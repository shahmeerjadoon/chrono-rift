#include "chrono_shm.hpp"

#include <SFML/Graphics.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static SharedMemory* gShm = nullptr;

static void onStunInterrupt(int) {
    sigset_t suspendMask;
    sigfillset(&suspendMask);
    sigdelset(&suspendMask, SIGUSR2);
    sigsuspend(&suspendMask);
}

static void onStunWake(int) {
    (void)0;
}

static void installHipStunHandlers() {
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = onStunInterrupt;
    sigemptyset(&sa1.sa_mask);
    sigaddset(&sa1.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sa1, nullptr);

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = onStunWake;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGUSR2, &sa2, nullptr);
}

/* ---- key queue (main thread enqueues from SFML; player threads block here) ---- */
static pthread_mutex_t gKeyMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gKeyCv = PTHREAD_COND_INITIALIZER;
static std::deque<int> gKeyQueue;
static int gUiShutdown = 0;

static void requestUiShutdown() {
    pthread_mutex_lock(&gKeyMu);
    gUiShutdown = 1;
    pthread_cond_broadcast(&gKeyCv);
    pthread_mutex_unlock(&gKeyMu);
}

static void enqueueKey(int c) {
    if (c < 0) return;
    pthread_mutex_lock(&gKeyMu);
    if (gKeyQueue.size() < 128) gKeyQueue.push_back(c);
    pthread_cond_signal(&gKeyCv);
    pthread_mutex_unlock(&gKeyMu);
}

static int dequeueKeyBlocking() {
    pthread_mutex_lock(&gKeyMu);
    while (gKeyQueue.empty() && !gUiShutdown) pthread_cond_wait(&gKeyCv, &gKeyMu);
    if (gKeyQueue.empty()) {
        pthread_mutex_unlock(&gKeyMu);
        return -1;
    }
    int c = gKeyQueue.front();
    gKeyQueue.pop_front();
    pthread_mutex_unlock(&gKeyMu);
    return c;
}

static void drainKeyQueue() {
    pthread_mutex_lock(&gKeyMu);
    gKeyQueue.clear();
    pthread_mutex_unlock(&gKeyMu);
}

/* ---- JRPG menu cursor (WASD / arrows; Enter/Space/Z confirm) — main thread + render read ---- */
static pthread_mutex_t gInputStateMu = PTHREAD_MUTEX_INITIALIZER;
static int gMenuNavEnabled = 0;
static int gActionMenuIndex = 0;

static void setMenuNavEnabled(int on) {
    pthread_mutex_lock(&gInputStateMu);
    gMenuNavEnabled = on ? 1 : 0;
    if (on) gActionMenuIndex = 0;
    pthread_mutex_unlock(&gInputStateMu);
}

/* 2×4 grid: cols 0–1, rows 0–3. W/S change row (wrap), A/D change column. */
static void menuNavGrid(int dRow, int dCol) {
    pthread_mutex_lock(&gInputStateMu);
    if (!gMenuNavEnabled) {
        pthread_mutex_unlock(&gInputStateMu);
        return;
    }
    int i = gActionMenuIndex;
    int row = i / 2;
    int col = i % 2;
    if (dRow < 0) {
        if (row > 0)
            i -= 2;
        else
            i += 6;
    } else if (dRow > 0) {
        if (row < 3)
            i += 2;
        else
            i -= 6;
    }
    if (dCol < 0) {
        if (col == 1) i -= 1;
    } else if (dCol > 0) {
        if (col == 0) i += 1;
    }
    gActionMenuIndex = i;
    pthread_mutex_unlock(&gInputStateMu);
}

static int menuNavPickChar() {
    static const char kMenuPick[] = "1234567q";
    pthread_mutex_lock(&gInputStateMu);
    int i = gActionMenuIndex;
    pthread_mutex_unlock(&gInputStateMu);
    if (i < 0 || i > 7) i = 0;
    return (unsigned char)kMenuPick[i];
}

static void addMonoMs(struct timespec* ts, int ms);

/* ---- Target pick (after Attack/Exhaust/Weapon): WASD / arrows cycle, Enter/Space/Z confirms ---- */
static int gTargetNavEnabled = 0;
static int gTargetList[cr::maxEnemies];
static int gTargetCount = 0;
static int gTargetCursor = 0;

static void setTargetNavEnabled(int on) {
    pthread_mutex_lock(&gInputStateMu);
    gTargetNavEnabled = on ? 1 : 0;
    if (!on) {
        gTargetCount = 0;
        gTargetCursor = 0;
    }
    pthread_mutex_unlock(&gInputStateMu);
}

static void targetNavDelta(int delta, SharedMemory* shm) {
    pthread_mutex_lock(&gInputStateMu);
    if (!gTargetNavEnabled || gTargetCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return;
    }
    gTargetCursor = (gTargetCursor + delta + gTargetCount) % gTargetCount;
    int slot = gTargetList[gTargetCursor];
    pthread_mutex_unlock(&gInputStateMu);
    pthread_mutex_lock(&shm->stateLock);
    shm->uiTargetEnemySlot = slot;
    clock_gettime(CLOCK_MONOTONIC, &shm->uiTargetEndMono);
    addMonoMs(&shm->uiTargetEndMono, 1600);
    pthread_mutex_unlock(&shm->stateLock);
}

static int targetNavConfirmSlot() {
    pthread_mutex_lock(&gInputStateMu);
    if (!gTargetNavEnabled || gTargetCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return -1;
    }
    int slot = gTargetList[gTargetCursor];
    pthread_mutex_unlock(&gInputStateMu);
    return slot;
}

/* ---- Weapon pick: owned weapons + "<< Back"; Enter confirms (digits still work) ---- */
static int gWeaponNavEnabled = 0;
static int gWeaponList[weaponIds::count];
static int gWeaponOwnCount = 0;
static int gWeaponCount = 0; /* own + back row */
static int gWeaponCursor = 0;

static void setWeaponNavEnabled(int on) {
    pthread_mutex_lock(&gInputStateMu);
    gWeaponNavEnabled = on ? 1 : 0;
    if (!on) {
        gWeaponOwnCount = 0;
        gWeaponCount = 0;
        gWeaponCursor = 0;
    }
    pthread_mutex_unlock(&gInputStateMu);
}

static void weaponNavDelta(int delta) {
    pthread_mutex_lock(&gInputStateMu);
    if (!gWeaponNavEnabled || gWeaponCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return;
    }
    gWeaponCursor = (gWeaponCursor + delta + gWeaponCount) % gWeaponCount;
    pthread_mutex_unlock(&gInputStateMu);
}

/* Returns weapon id, or -2 = back to CMD, or -1 = invalid */
static int weaponNavConfirmId() {
    pthread_mutex_lock(&gInputStateMu);
    if (!gWeaponNavEnabled || gWeaponCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return -1;
    }
    if (gWeaponCursor >= gWeaponOwnCount) {
        pthread_mutex_unlock(&gInputStateMu);
        return -2;
    }
    int id = gWeaponList[gWeaponCursor];
    pthread_mutex_unlock(&gInputStateMu);
    return id;
}

/* ---- Swap in (long-term storage slot pick): same UX pattern as weapon submenu ---- */
static int gSwapNavEnabled = 0;
static int gSwapLtsCount = 0;
static int gSwapCount = 0; /* lts rows + cancel */
static int gSwapCursor = 0;

static void setSwapNavEnabled(int on) {
    pthread_mutex_lock(&gInputStateMu);
    gSwapNavEnabled = on ? 1 : 0;
    if (!on) {
        gSwapLtsCount = 0;
        gSwapCount = 0;
        gSwapCursor = 0;
    }
    pthread_mutex_unlock(&gInputStateMu);
}

static void swapNavDelta(int delta) {
    pthread_mutex_lock(&gInputStateMu);
    if (!gSwapNavEnabled || gSwapCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return;
    }
    gSwapCursor = (gSwapCursor + delta + gSwapCount) % gSwapCount;
    pthread_mutex_unlock(&gInputStateMu);
}

/* Returns LTS index 0..count-1, or -2 = cancel/back, or -1 = invalid */
static int swapNavConfirmLtsIndex() {
    pthread_mutex_lock(&gInputStateMu);
    if (!gSwapNavEnabled || gSwapCount <= 0) {
        pthread_mutex_unlock(&gInputStateMu);
        return -1;
    }
    if (gSwapCursor >= gSwapLtsCount) {
        pthread_mutex_unlock(&gInputStateMu);
        return -2;
    }
    int ix = gSwapCursor;
    pthread_mutex_unlock(&gInputStateMu);
    return ix;
}

static const int kSwapCommitBase = 0x4000;

/* ---- Ultimate submenu: Cast vs Back (WASD / Enter) ---- */
static int gUltMenuEnabled = 0;
static int gUltCursor = 0;
static const int gUltMenuRows = 2;

static void setUltMenuEnabled(int on) {
    pthread_mutex_lock(&gInputStateMu);
    gUltMenuEnabled = on ? 1 : 0;
    if (!on) gUltCursor = 0;
    pthread_mutex_unlock(&gInputStateMu);
}

static void ultMenuDelta(int delta) {
    pthread_mutex_lock(&gInputStateMu);
    if (!gUltMenuEnabled) {
        pthread_mutex_unlock(&gInputStateMu);
        return;
    }
    gUltCursor = (gUltCursor + delta + gUltMenuRows) % gUltMenuRows;
    pthread_mutex_unlock(&gInputStateMu);
}

/* 1 = cast ultimate, -2 = back, -1 = invalid */
static int ultMenuConfirmAction() {
    pthread_mutex_lock(&gInputStateMu);
    if (!gUltMenuEnabled) {
        pthread_mutex_unlock(&gInputStateMu);
        return -1;
    }
    int c = gUltCursor;
    pthread_mutex_unlock(&gInputStateMu);
    if (c == 1) return -2;
    return 1;
}

/* ---- footer / prompt overlay (player threads write; render thread reads) ---- */
static pthread_mutex_t gPromptMu = PTHREAD_MUTEX_INITIALIZER;
static char gPromptLine0[256];
static char gPromptLine1[256];
static char gPromptLine2[256];
static char gOwnedLine[256];
static int gPromptLines = 0;

// big eclipse popup visible — render thread reads, player thread flips it
static pthread_mutex_t gEclipseModalMu = PTHREAD_MUTEX_INITIALIZER;
static int gEclipsePickupModalOn = 0;

static void setEclipsePickupModalVisible(int on) {
    pthread_mutex_lock(&gEclipseModalMu);
    gEclipsePickupModalOn = on ? 1 : 0;
    pthread_mutex_unlock(&gEclipseModalMu);
}

static int eclipsePickupModalIsVisible() {
    pthread_mutex_lock(&gEclipseModalMu);
    int v = gEclipsePickupModalOn;
    pthread_mutex_unlock(&gEclipseModalMu);
    return v;
}

static void setUiPrompt(const char* a, const char* b = "", const char* c = "") {
    pthread_mutex_lock(&gPromptMu);
    strncpy(gPromptLine0, a ? a : "", sizeof(gPromptLine0) - 1);
    gPromptLine0[sizeof(gPromptLine0) - 1] = '\0';
    strncpy(gPromptLine1, b ? b : "", sizeof(gPromptLine1) - 1);
    gPromptLine1[sizeof(gPromptLine1) - 1] = '\0';
    strncpy(gPromptLine2, c ? c : "", sizeof(gPromptLine2) - 1);
    gPromptLine2[sizeof(gPromptLine2) - 1] = '\0';
    if (!a || !a[0]) {
        gPromptLines = 0;
        gOwnedLine[0] = '\0';
    } else if (!b || !b[0])
        gPromptLines = 1;
    else if (!c || !c[0])
        gPromptLines = 2;
    else
        gPromptLines = 3;
    pthread_mutex_unlock(&gPromptMu);
}

static void setHeroTurnBanner(int heroIdx, const Entity* p) {
    (void)heroIdx;
    (void)p;
    pthread_mutex_lock(&gPromptMu);
    /* CMD grid + portrait already show whose turn it is; skip footer banner/gear to avoid overlap. */
    gPromptLine0[0] = '\0';
    gPromptLine1[0] = '\0';
    gPromptLine2[0] = '\0';
    gPromptLines = 0;
    gOwnedLine[0] = '\0';
    pthread_mutex_unlock(&gPromptMu);
}

static int gFrame = 0;
static float gCtxHintFade = 0.f;
static float gWeaponPanelSlide = 0.f;
static float gWeaponScrollPx = 0.f;
static int gPrevWeaponSubmenu = 0;
static float gSwapPanelSlide = 0.f;
static float gSwapScrollPx = 0.f;
static int gPrevSwapSubmenu = 0;

static int primaryEquippedWeaponId(const Inventory* inv) {
    for (int s = 0; s < cr::invSize; s++) {
        int id = inv->slots[s];
        if (id >= 0 && id < weaponIds::count) return id;
    }
    return weaponIds::none;
}

static int invSlotForWeapon(const Inventory* inv, int wid) {
    for (int s = 0; s < cr::invSize; s++)
        if (inv->slots[s] == wid) return s;
    return -1;
}

/* Battle menu: smooth selection rect + cursor bounce (retro, not flashy). */
static int countAliveEnemies(const SharedMemory* shm) {
    int n = 0;
    for (int i = 0; i < cr::maxEnemies; i++)
        if (shm->enemies[i].isAlive) n++;
    return n;
}

static void addMonoMs(struct timespec* ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int monoBeforeEnd(const struct timespec* end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != end->tv_sec) return now.tv_sec < end->tv_sec;
    return now.tv_nsec < end->tv_nsec;
}

static double monoAsDouble(const struct timespec* ts) {
    return (double)ts->tv_sec + 1e-9 * (double)ts->tv_nsec;
}

static float monoRemainMs(const struct timespec* end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > end->tv_sec || (now.tv_sec == end->tv_sec && now.tv_nsec >= end->tv_nsec)) return 0.f;
    double ns = (double)(end->tv_sec - now.tv_sec) * 1e9 + (double)(end->tv_nsec - now.tv_nsec);
    return (float)(ns / 1e6);
}

static void sampleDecayShake(const struct timespec* endMono, float mag, float* ox, float* oy) {
    float rem = monoRemainMs(endMono);
    if (rem <= 0.f) {
        *ox = *oy = 0.f;
        return;
    }
    const float dur = 480.f;
    float age = 1.f - std::clamp(rem / dur, 0.f, 1.f);
    float w = age * age;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double ph = monoAsDouble(&now) * 55.0;
    *ox = (float)(std::sin(ph) * (double)mag * w);
    *oy = (float)(std::cos(ph * 1.27) * (double)mag * 0.62 * w);
}

/* Normalized 0..1 progress through fxProj window, or -1 if inactive. */
static float fxProjNormT(const SharedMemory* s) {
    if (s->fxProjKind == 0 || !monoBeforeEnd(&s->fxProjEndMono)) return -1.f;
    double a = monoAsDouble(&s->fxProjStartMono);
    double b = monoAsDouble(&s->fxProjEndMono);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double n = monoAsDouble(&now);
    double t = (b > a) ? (n - a) / (b - a) : 1.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (float)t;
}

static int mapKeyboardToAscii(sf::Keyboard::Key k, bool shift) {
    using K = sf::Keyboard;
    switch (k) {
        case K::Num0:
        case K::Numpad0:
            return '0';
        case K::Num1:
        case K::Numpad1:
            return '1';
        case K::Num2:
        case K::Numpad2:
            return '2';
        case K::Num3:
        case K::Numpad3:
            return '3';
        case K::Num4:
        case K::Numpad4:
            return '4';
        case K::Num5:
        case K::Numpad5:
            return '5';
        case K::Num6:
        case K::Numpad6:
            return '6';
        case K::Num7:
        case K::Numpad7:
            return '7';
        case K::Num8:
        case K::Numpad8:
            return '8';
        case K::Num9:
        case K::Numpad9:
            return '9';
        case K::Q:
            return shift ? 'Q' : 'q';
        case K::W:
            return shift ? 'W' : 'w';
        case K::E:
            return shift ? 'E' : 'e';
        case K::A:
            return shift ? 'A' : 'a';
        case K::S:
            return shift ? 'S' : 's';
        case K::D:
            return shift ? 'D' : 'd';
        case K::H:
            return shift ? 'H' : 'h';
        case K::U:
            return shift ? 'U' : 'u';
        case K::X:
            return shift ? 'X' : 'x';
        case K::I:
            return shift ? 'I' : 'i';
        case K::Y:
            return shift ? 'Y' : 'y';
        case K::N:
            return shift ? 'N' : 'n';
        default:
            return -1;
    }
}

static bool loadUiFont(sf::Font& out) {
    static const char* paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    };
    for (const char* p : paths) {
        if (out.loadFromFile(p)) return true;
    }
    return false;
}

static void drawText(sf::RenderTarget& tgt, const sf::Font& font, unsigned size, const char* utf8, float x,
                     float y, sf::Color fill) {
    sf::Text t;
    t.setFont(font);
    t.setString(utf8);
    t.setCharacterSize(size);
    t.setFillColor(fill);
    t.setPosition(x, y);
    tgt.draw(t);
}

static void drawTextItalic(sf::RenderTarget& tgt, const sf::Font& font, unsigned size, const char* utf8, float x,
                           float y, sf::Color fill) {
    sf::Text t;
    t.setFont(font);
    t.setString(utf8);
    t.setCharacterSize(size);
    t.setFillColor(fill);
    t.setStyle(sf::Text::Italic);
    t.setPosition(x, y);
    tgt.draw(t);
}

/* Resolution scale for HUD + battlefield labels (set each frame in renderFrame). */
static float gUiScale = 1.f;

static void drawTextShadow(sf::RenderTarget& tgt, const sf::Font& font, unsigned size, const char* utf8, float x,
                           float y, sf::Color fill, sf::Color shadow) {
    float o = std::max(1.f, 2.f * gUiScale);
    drawText(tgt, font, size, utf8, x + o, y + o, shadow);
    drawText(tgt, font, size, utf8, x, y, fill);
}

/* Typography scale: headers > body > stats > labels > meta (read top-to-bottom priority). */
static const unsigned kTypeHead = 12;
static const unsigned kTypeBody = 11;
static const unsigned kTypeStat = 11;
static const unsigned kTypeStatMax = 8;
static const unsigned kTypeLabel = 9;
static const unsigned kTypeMeta = 8;
static const unsigned kTypeHint = 9;
static const unsigned kTypeEntity = 12;

static unsigned uiSize(unsigned base) {
    return (unsigned)std::max(7u, (unsigned)std::lround((float)base * gUiScale));
}

static const sf::Color kToneHdr(255, 250, 235);
static const sf::Color kToneBody(240, 246, 238);
static const sf::Color kToneStat(255, 254, 250);
static const sf::Color kToneMuted(175, 195, 175);
static const sf::Color kToneFaint(150, 168, 152);

static float measureTextWidth(const sf::Font& font, unsigned size, const char* utf8) {
    sf::Text t;
    t.setFont(font);
    t.setCharacterSize(size);
    t.setString(utf8);
    return t.getLocalBounds().width;
}

static void fmtNumCompact(char* out, size_t n, int v) {
    if (v >= 1000000)
        snprintf(out, n, "%.1fM", (double)v / 1e6);
    else if (v >= 100000)
        snprintf(out, n, "%dk", v / 1000);
    else if (v >= 10000)
        snprintf(out, n, "%.1fk", (double)v / 1000.0);
    else
        snprintf(out, n, "%d", v);
}

static void drawBar(sf::RenderTarget& tgt, float x, float y, float w, float h, float frac, sf::Color full,
                    sf::Color empty) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float fw = w * frac;
    sf::RectangleShape e({w, h});
    e.setPosition(x, y);
    e.setFillColor(empty);
    tgt.draw(e);
    if (fw > 0.5f) {
        sf::RectangleShape f({fw, h});
        f.setPosition(x, y);
        f.setFillColor(full);
        tgt.draw(f);
    }
}

static void drawCtFillPanel(sf::RenderTarget& tgt, sf::FloatRect r, sf::Color fill) {
    sf::RectangleShape bg({r.width, r.height});
    bg.setPosition(r.left, r.top);
    bg.setFillColor(fill);
    tgt.draw(bg);
}

static void drawCtBorder(sf::RenderTarget& tgt, sf::FloatRect r, sf::Color border, float t) {
    sf::RectangleShape a({r.width, t});
    a.setPosition(r.left, r.top);
    a.setFillColor(border);
    tgt.draw(a);
    sf::RectangleShape b({r.width, t});
    b.setPosition(r.left, r.top + r.height - t);
    b.setFillColor(border);
    tgt.draw(b);
    sf::RectangleShape c({t, r.height});
    c.setPosition(r.left, r.top);
    c.setFillColor(border);
    tgt.draw(c);
    sf::RectangleShape d({t, r.height});
    d.setPosition(r.left + r.width - t, r.top);
    d.setFillColor(border);
    tgt.draw(d);
}

/* 4px grid + shared chrome — one retro panel language for HUD + subpanels. */
static const int kUiGrid = 4;

static float uiSnap(float v) {
    return std::floor(v / (float)kUiGrid + 0.5f) * (float)kUiGrid;
}

static const sf::Color kRetroShadow(0, 0, 0, 88);
static const sf::Color kRetroEdgeDark(36, 40, 38);
static const sf::Color kRetroEdgeLight(208, 216, 198);
static const sf::Color kRetroInnerHi(255, 255, 255, 42);

static void drawRetroWindow(sf::RenderTarget& tgt, sf::FloatRect r, sf::Color fill, bool dropShadow, bool tripleRim) {
    float x = uiSnap(r.left);
    float y = uiSnap(r.top);
    float w = std::max((float)(kUiGrid * 3), r.width);
    float h = std::max((float)(kUiGrid * 3), r.height);
    if (dropShadow) {
        sf::RectangleShape sh({w, h});
        sh.setPosition(x + (float)kUiGrid, y + (float)kUiGrid);
        sh.setFillColor(kRetroShadow);
        tgt.draw(sh);
    }
    drawCtFillPanel(tgt, {x, y, w, h}, fill);
    drawCtBorder(tgt, {x, y, w, h}, kRetroEdgeDark, 1.f);
    if (w > 4.f && h > 4.f)
        drawCtBorder(tgt, {x + 1.f, y + 1.f, w - 2.f, h - 2.f}, kRetroEdgeLight, 1.f);
    if (tripleRim && w > (float)(kUiGrid * 3) && h > (float)(kUiGrid * 3))
        drawCtBorder(tgt, {x + 3.f, y + 3.f, w - 6.f, h - 6.f}, kRetroInnerHi, 1.f);
}

static const sf::Color kGaugePartyHpFill(76, 152, 98);
static const sf::Color kGaugePartyHpTrack(26, 40, 34);
static const sf::Color kGaugePartyStFill(186, 158, 72);
static const sf::Color kGaugePartyStTrack(38, 34, 26);
static const sf::Color kGaugeFoeHpFill(172, 70, 90);
static const sf::Color kGaugeFoeHpTrack(36, 26, 30);
static const sf::Color kGaugeFoeStFill(190, 162, 76);
static const sf::Color kGaugeFoeStTrack(40, 36, 28);

/* JRPG-style gauge: outer bezel, inset track, soft fill, 1px hi/lo for pixel depth. */
/* Compact SNES-style bar for party panel: thin bezel, no drop shadow clutter. */
static void drawThinGauge(sf::RenderTarget& tgt, float x, float y, float w, float h, float frac, sf::Color fill,
                          sf::Color track) {
    if (w < 4.f || h < 1.5f) return;
    drawCtFillPanel(tgt, {x - 1.f, y - 1.f, w + 2.f, h + 2.f}, sf::Color(12, 16, 14));
    drawCtBorder(tgt, {x - 1.f, y - 1.f, w + 2.f, h + 2.f}, kRetroEdgeDark, 1.f);
    drawBar(tgt, x, y, w, h, frac, fill, track);
    if (h >= 2.f) {
        sf::RectangleShape hi({w, 1.f});
        hi.setPosition(x, y);
        hi.setFillColor(sf::Color(255, 255, 255, 28));
        tgt.draw(hi);
    }
}

/* ---- Gothic HUD overlays (weapon-popup vibe): gold #f5c018, dark panels, smooth HP ---- */
static const sf::Color kUiGold(245, 192, 24);

static unsigned uiOverlaySize(unsigned base) {
    return (unsigned)std::max(9u, (unsigned)std::lround((float)base * gUiScale * 1.22f));
}

static float gHudHpSmoothParty[cr::maxPlayers];
static int gHudHpSmoothReady = 0;

static void hudUpdateHpSmoothing(const SharedMemory* s) {
    if (!gHudHpSmoothReady) {
        for (int i = 0; i < cr::maxPlayers; i++) {
            const Entity* p = &s->players[i];
            gHudHpSmoothParty[i] = p->maxHp > 0 ? (float)p->hp / (float)p->maxHp : 0.f;
        }
        gHudHpSmoothReady = 1;
        return;
    }
    const float k = 0.16f;
    for (int i = 0; i < s->numPlayers; i++) {
        const Entity* p = &s->players[i];
        float t = p->maxHp > 0 ? (float)p->hp / (float)p->maxHp : 0.f;
        gHudHpSmoothParty[i] += (t - gHudHpSmoothParty[i]) * k;
    }
}

static sf::Color hudHpMixColor(float frac01) {
    frac01 = std::clamp(frac01, 0.f, 1.f);
    const sf::Color G(52, 196, 92), Y(245, 205, 58), R(216, 48, 52);
    if (frac01 >= 0.5f) {
        float t = (frac01 - 0.5f) * 2.f;
        return sf::Color((unsigned char)(Y.r + (G.r - Y.r) * t), (unsigned char)(Y.g + (G.g - Y.g) * t),
                         (unsigned char)(Y.b + (G.b - Y.b) * t));
    }
    float t = frac01 * 2.f;
    return sf::Color((unsigned char)(R.r + (Y.r - R.r) * t), (unsigned char)(R.g + (Y.g - R.g) * t),
                       (unsigned char)(R.b + (Y.b - R.b) * t));
}

static void drawHudHpTrackAndFill(sf::RenderTarget& tgt, float x, float y, float w, float h, float smoothFrac01) {
    const float inset = 1.f;
    if (w < 6.f || h < 4.f) return;
    sf::RectangleShape track({w, h});
    track.setPosition(x, y);
    track.setFillColor(sf::Color(18, 18, 18));
    track.setOutlineThickness(1.f);
    track.setOutlineColor(sf::Color(6, 6, 8));
    tgt.draw(track);
    float iw = w - 2.f * inset;
    float ih = h - 2.f * inset;
    float fw = iw * std::clamp(smoothFrac01, 0.f, 1.f);
    if (fw < 0.5f) return;
    sf::Color base = hudHpMixColor(std::clamp(smoothFrac01, 0.f, 1.f));
    sf::RectangleShape fill({fw, ih});
    fill.setPosition(x + inset, y + inset);
    fill.setFillColor(base);
    tgt.draw(fill);
    sf::RectangleShape hi({fw, std::max(1.f, ih * 0.38f)});
    hi.setPosition(x + inset, y + inset);
    hi.setFillColor(sf::Color(255, 255, 255, 38));
    tgt.draw(hi);
}

static void drawHudStaminaBar(sf::RenderTarget& tgt, float x, float y, float w, float h, float frac01) {
    if (w < 6.f || h < 3.f) return;
    sf::RectangleShape track({w, h});
    track.setPosition(x, y);
    track.setFillColor(sf::Color(18, 18, 18));
    track.setOutlineThickness(1.f);
    track.setOutlineColor(sf::Color(6, 6, 8));
    tgt.draw(track);
    float fw = (w - 2.f) * std::clamp(frac01, 0.f, 1.f);
    if (fw < 0.5f) return;
    sf::RectangleShape fill({fw, h - 2.f});
    fill.setPosition(x + 1.f, y + 1.f);
    fill.setFillColor(sf::Color(212, 178, 72));
    tgt.draw(fill);
}

static void drawUiGoldGlowBorder(sf::RenderTarget& tgt, sf::FloatRect r) {
    sf::RectangleShape glow({r.width + 4.f, r.height + 4.f});
    glow.setPosition(r.left - 2.f, r.top - 2.f);
    glow.setFillColor(sf::Color::Transparent);
    glow.setOutlineThickness(1.f);
    glow.setOutlineColor(sf::Color(245, 192, 24, 48));
    tgt.draw(glow);
    sf::RectangleShape edge({r.width, r.height});
    edge.setPosition(r.left, r.top);
    edge.setFillColor(sf::Color::Transparent);
    edge.setOutlineThickness(1.f);
    edge.setOutlineColor(kUiGold);
    tgt.draw(edge);
}

static void drawUiPanelDark(sf::RenderTarget& tgt, sf::FloatRect r) {
    sf::RectangleShape bg({r.width, r.height});
    bg.setPosition(r.left, r.top);
    bg.setFillColor(sf::Color(13, 13, 13, 228));
    tgt.draw(bg);
    drawUiGoldGlowBorder(tgt, r);
}

static void drawGoldSectionRule(sf::RenderTarget& tgt, float x, float y, float w) {
    sf::RectangleShape glow({w, 2.f});
    glow.setPosition(x, y);
    glow.setFillColor(sf::Color(245, 192, 24, 26));
    tgt.draw(glow);
    sf::RectangleShape line({w, 1.f});
    line.setPosition(x, y + 0.5f);
    line.setFillColor(sf::Color(245, 192, 24, 175));
    tgt.draw(line);
}

static float drawHudStatChip(sf::RenderTarget& tgt, const sf::Font& font, float x, float y, const char* labelUpper,
                             const char* valueStr, sf::Color valueCol) {
    unsigned szLab = uiOverlaySize(kTypeLabel);
    unsigned szVal = uiOverlaySize(kTypeBody);
    sf::Color dimBrack(245, 192, 24, 95);
    sf::Color dimLab(148, 146, 138);
    float x0 = x;
    drawText(tgt, font, szLab, "[", x0, y, dimBrack);
    x0 += measureTextWidth(font, szLab, "[") + 3.f;
    drawText(tgt, font, szLab, labelUpper, x0, y, dimLab);
    x0 += measureTextWidth(font, szLab, labelUpper) + 5.f;
    drawText(tgt, font, szVal, valueStr, x0, y, valueCol);
    x0 += measureTextWidth(font, szVal, valueStr) + 3.f;
    drawText(tgt, font, szLab, "]", x0, y, dimBrack);
    x0 += measureTextWidth(font, szLab, "]");
    return x0 - x + 12.f;
}

static void drawSlashHpNumbers(sf::RenderTarget& tgt, const sf::Font& font, float rightX, float yBaseline,
                               const char* cur, const char* mx, sf::Color numCol) {
    unsigned sz = uiOverlaySize(kTypeMeta);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s / %s", cur, mx);
    float tw = measureTextWidth(font, sz, buf);
    drawText(tgt, font, sz, buf, rightX - tw, yBaseline, numCol);
}

/* gold highlight bar — drawn shapes not unicode arrows */
static void drawCmdRowChrome(sf::RenderTarget& tgt, sf::FloatRect row, bool selected, bool hover) {
    if (selected) {
        sf::RectangleShape selBg({row.width, row.height});
        selBg.setPosition(row.left, row.top);
        selBg.setFillColor(sf::Color(245, 192, 24, 34));
        tgt.draw(selBg);
    } else if (hover) {
        sf::RectangleShape hg({row.width, row.height});
        hg.setPosition(row.left, row.top);
        hg.setFillColor(sf::Color(245, 192, 24, 16));
        tgt.draw(hg);
    }
    if (!selected) return;
    sf::RectangleShape bar({3.f, row.height - 2.f});
    bar.setPosition(row.left + 1.f, row.top + 1.f);
    bar.setFillColor(kUiGold);
    tgt.draw(bar);
    sf::ConvexShape tri;
    tri.setPointCount(3);
    float cy = row.top + row.height * 0.5f;
    tri.setPoint(0, {row.left + 5.f, cy - 4.f});
    tri.setPoint(1, {row.left + 5.f, cy + 4.f});
    tri.setPoint(2, {row.left + 11.f, cy});
    tri.setFillColor(kUiGold);
    tgt.draw(tri);
}

/* tiny arrow thing for swap header */
static void drawGoldRightChevronTip(sf::RenderTarget& tgt, float tipX, float centerY, float halfH, const sf::Color& c) {
    sf::ConvexShape tri;
    tri.setPointCount(3);
    tri.setPoint(0, {tipX - 6.f, centerY - halfH});
    tri.setPoint(1, {tipX - 6.f, centerY + halfH});
    tri.setPoint(2, {tipX, centerY});
    tri.setFillColor(c);
    tgt.draw(tri);
}

/* says scroll up/down in ascii — unicode triangles looked gross */
static void drawScrollArrowUp(sf::RenderTarget& tgt, float cx, float tipY, float span, const sf::Color& c) {
    sf::ConvexShape tri;
    tri.setPointCount(3);
    tri.setPoint(0, {cx, tipY});
    tri.setPoint(1, {cx - span * 0.55f, tipY + span});
    tri.setPoint(2, {cx + span * 0.55f, tipY + span});
    tri.setFillColor(c);
    tgt.draw(tri);
}

static void drawScrollArrowDown(sf::RenderTarget& tgt, float cx, float tipY, float span, const sf::Color& c) {
    sf::ConvexShape tri;
    tri.setPointCount(3);
    tri.setPoint(0, {cx, tipY});
    tri.setPoint(1, {cx - span * 0.55f, tipY - span});
    tri.setPoint(2, {cx + span * 0.55f, tipY - span});
    tri.setFillColor(c);
    tgt.draw(tri);
}

/* Text-only tag — avoids stray outline boxes from RectangleShape + GPU quirks. */
static void drawEquippedTag(sf::RenderTarget& tgt, const sf::Font& font, float x, float y, float fade01) {
    unsigned sz = uiOverlaySize(kTypeMeta);
    sf::Uint8 a = (sf::Uint8)(std::clamp(fade01, 0.f, 1.f) * 245.f);
    drawText(tgt, font, sz, "[EQUIPPED]", x, y, sf::Color(245, 205, 96, a));
}

static void drawTargetPickHintBar(sf::RenderTarget& tgt, const sf::Font& font, float margin, float W, float bottomY,
                                  float hintH, float fade01) {
    if (fade01 < 0.02f) return;
    float y0 = bottomY - hintH;
    sf::Uint8 aBg = (sf::Uint8)(std::clamp(fade01, 0.f, 1.f) * 218.f);
    sf::RectangleShape bg({W - 2.f * margin, hintH});
    bg.setPosition(margin, y0);
    bg.setFillColor(sf::Color(6, 6, 8, aBg));
    tgt.draw(bg);
    sf::RectangleShape goldLine({W - 2.f * margin, 1.f});
    goldLine.setPosition(margin, bottomY - 1.f);
    goldLine.setFillColor(sf::Color(245, 192, 24, (sf::Uint8)((float)aBg * 0.65f)));
    tgt.draw(goldLine);

    const char* msg = "* Select target - Enter to confirm";
    unsigned sz = uiOverlaySize(kTypeHint);
    float tw = measureTextWidth(font, sz, msg);
    float cx = margin + (W - 2.f * margin - tw) * 0.5f;
    float cy = y0 + (hintH - (float)sz) * 0.34f;
    sf::Uint8 txA = (sf::Uint8)(std::clamp(fade01, 0.f, 1.f) * 255.f);
    drawTextItalic(tgt, font, sz, msg, cx, cy, sf::Color(196, 162, 88, txA));
}

static void drawSwapPickHintBar(sf::RenderTarget& tgt, const sf::Font& font, float margin, float W, float bottomY,
                                float hintH, float fade01, const char* msg) {
    if (fade01 < 0.02f || !msg) return;
    float y0 = bottomY - hintH;
    sf::Uint8 aBg = (sf::Uint8)(std::clamp(fade01, 0.f, 1.f) * 218.f);
    sf::RectangleShape bg({W - 2.f * margin, hintH});
    bg.setPosition(margin, y0);
    bg.setFillColor(sf::Color(6, 6, 8, aBg));
    tgt.draw(bg);
    sf::RectangleShape goldLine({W - 2.f * margin, 1.f});
    goldLine.setPosition(margin, bottomY - 1.f);
    goldLine.setFillColor(sf::Color(245, 192, 24, (sf::Uint8)((float)aBg * 0.65f)));
    tgt.draw(goldLine);

    unsigned sz = uiOverlaySize(kTypeHint);
    float tw = measureTextWidth(font, sz, msg);
    float cx = margin + (W - 2.f * margin - tw) * 0.5f;
    float cy = y0 + (hintH - (float)sz) * 0.34f;
    sf::Uint8 txA = (sf::Uint8)(std::clamp(fade01, 0.f, 1.f) * 255.f);
    drawTextItalic(tgt, font, sz, msg, cx, cy, sf::Color(196, 162, 88, txA));
}

static void drawCornerBrackets(sf::RenderTarget& tgt, float x, float y, float w, float h, sf::Color c, float L,
                               float thick) {
    sf::RectangleShape q;
    q.setFillColor(c);
    // top-left
    q.setSize({L, thick});
    q.setPosition(x, y);
    tgt.draw(q);
    q.setSize({thick, L});
    q.setPosition(x, y);
    tgt.draw(q);
    // top-right
    q.setSize({L, thick});
    q.setPosition(x + w - L, y);
    tgt.draw(q);
    q.setSize({thick, L});
    q.setPosition(x + w - thick, y);
    tgt.draw(q);
    // bottom-left
    q.setSize({L, thick});
    q.setPosition(x, y + h - thick);
    tgt.draw(q);
    q.setSize({thick, L});
    q.setPosition(x, y + h - L);
    tgt.draw(q);
    // bottom-right
    q.setSize({L, thick});
    q.setPosition(x + w - L, y + h - thick);
    tgt.draw(q);
    q.setSize({thick, L});
    q.setPosition(x + w - thick, y + h - L);
    tgt.draw(q);
}

struct ArenaLayout {
    float pCx[cr::maxPlayers];
    float pCy[cr::maxPlayers];
    float pR[cr::maxPlayers];
    float eCx[cr::maxEnemies];
    float eCy[cr::maxEnemies];
    float eR[cr::maxEnemies];
};

static sf::Uint8 lerpU8(int a, int b, float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    return (sf::Uint8)((float)a + (float)(b - a) * t);
}

static void drawSkyGradient(sf::RenderTarget& tgt, float left, float top, float w, float horizonY) {
    const int bands = 8;
    float h = horizonY - top;
    for (int i = 0; i < bands; i++) {
        float t0 = (float)i / (float)bands;
        float t1 = (float)(i + 1) / (float)bands;
        sf::Uint8 r = lerpU8(72, 168, t0 * 0.65f + 0.2f);
        sf::Uint8 g = lerpU8(118, 214, t0 * 0.55f + 0.25f);
        sf::Uint8 b = lerpU8(188, 245, t0 * 0.5f + 0.35f);
        sf::RectangleShape band({w, (t1 - t0) * h + 0.5f});
        band.setPosition(left, top + t0 * h);
        band.setFillColor(sf::Color(r, g, b));
        tgt.draw(band);
    }
}

static void drawArenaSpotlight(sf::RenderTarget& tgt, float cx, float cy, float rx, float ry) {
    sf::CircleShape glow(rx);
    glow.setOrigin(rx, rx);
    glow.setPosition(cx, cy);
    glow.setScale(1.f, ry / rx);
    glow.setFillColor(sf::Color(255, 252, 235, 16));
    tgt.draw(glow);
}

/* Soft edge darken — ties the map to the frame without a flat “modern” overlay. */
static void drawBattleVignette(sf::RenderTarget& tgt, float left, float top, float w, float h) {
    if (w < 8.f || h < 8.f) return;
    const int bands = 5;
    for (int i = 0; i < bands; i++) {
        float t = (float)(i + 1) / (float)bands;
        sf::Uint8 a = (sf::Uint8)(22.f * t * t);
        sf::RectangleShape strip({w, 2.f});
        strip.setPosition(left, top + (float)i * 2.f);
        strip.setFillColor(sf::Color(8, 10, 12, a));
        tgt.draw(strip);
        strip.setPosition(left, top + h - (float)(i + 1) * 2.f);
        tgt.draw(strip);
    }
    for (int i = 0; i < bands; i++) {
        float t = (float)(i + 1) / (float)bands;
        sf::Uint8 a = (sf::Uint8)(18.f * t * t);
        sf::RectangleShape strip({2.f, h});
        strip.setPosition(left + (float)i * 2.f, top);
        strip.setFillColor(sf::Color(6, 8, 10, a));
        tgt.draw(strip);
        strip.setPosition(left + w - (float)(i + 1) * 2.f, top);
        tgt.draw(strip);
    }
}

static void drawUnitShadow(sf::RenderTarget& tgt, float cx, float footY, float radiusHint) {
    float rx = radiusHint * 0.72f;
    sf::CircleShape sh(rx);
    sh.setOrigin(rx, rx * 0.32f);
    sh.setPosition(cx, footY);
    sh.setScale(1.f, 0.42f);
    sh.setFillColor(sf::Color(6, 14, 8, 130));
    tgt.draw(sh);
}

static void drawFacingWedge(sf::RenderTarget& tgt, float cx, float cy, float r, bool faceRight, sf::Color fill) {
    sf::ConvexShape w;
    w.setPointCount(3);
    if (faceRight) {
        w.setPoint(0, {cx + r * 0.55f, cy});
        w.setPoint(1, {cx + r + 14.f, cy - 9.f});
        w.setPoint(2, {cx + r + 14.f, cy + 9.f});
    } else {
        w.setPoint(0, {cx - r * 0.55f, cy});
        w.setPoint(1, {cx - r - 14.f, cy - 9.f});
        w.setPoint(2, {cx - r - 14.f, cy + 9.f});
    }
    w.setFillColor(fill);
    tgt.draw(w);
}

/* Battle backdrop: Sprites/Route 1.png (full image; use 0×0 rect = entire texture). */
static const sf::IntRect kBattleMapTexRect(0, 0, 0, 0);

static bool loadBattlefieldMapTexture(sf::Texture& tex) {
    tex.setSmooth(false);
    const char* rel[] = {"Sprites/Route 1.png", "./Sprites/Route 1.png", nullptr};
    for (int i = 0; rel[i]; i++) {
        if (tex.loadFromFile(rel[i])) return true;
    }
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, (size_t)sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        std::string base(exe);
        size_t slash = base.rfind('/');
        if (slash != std::string::npos) base.resize(slash);
        if (tex.loadFromFile(base + "/Sprites/Route 1.png")) return true;
        if (tex.loadFromFile(base + "/../Sprites/Route 1.png")) return true;
    }
    return false;
}

static bool loadTextureFromSpritesDir(sf::Texture& tex, const char* leaf) {
    tex.setSmooth(false);
    char rel[320];
    snprintf(rel, sizeof(rel), "Sprites/%s", leaf);
    if (tex.loadFromFile(rel)) return true;
    snprintf(rel, sizeof(rel), "./Sprites/%s", leaf);
    if (tex.loadFromFile(rel)) return true;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, (size_t)sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        std::string base(exe);
        size_t slash = base.rfind('/');
        if (slash != std::string::npos) base.resize(slash);
        if (tex.loadFromFile(base + "/Sprites/" + leaf)) return true;
        if (tex.loadFromFile(base + "/../Sprites/" + leaf)) return true;
    }
    return false;
}

static void drawBattlefieldMap(sf::RenderTarget& tgt, const sf::Texture& tex, const sf::IntRect& srcRect, float left,
                               float top, float right, float bot, float anchorY01) {
    sf::IntRect src = srcRect;
    if (src.width <= 0 || src.height <= 0) {
        auto sz = tex.getSize();
        src = sf::IntRect(0, 0, (int)sz.x, (int)sz.y);
    }
    float dw = right - left;
    float dh = bot - top;
    float sw = (float)src.width;
    float sh = (float)src.height;
    if (dw < 1.f || dh < 1.f || sw < 1.f || sh < 1.f) return;
    if (anchorY01 < 0.f) anchorY01 = 0.f;
    if (anchorY01 > 1.f) anchorY01 = 1.f;
    float scale = std::max(dw / sw, dh / sh);
    sf::Sprite spr(tex);
    spr.setTextureRect(src);
    spr.setScale(scale, scale);
    float dispW = sw * scale;
    float dispH = sh * scale;
    spr.setPosition(left + (dw - dispW) * 0.5f, top + (dh - dispH) * anchorY01);
    tgt.draw(spr);
}

static float foeBlobRadius(const Entity& en, float arenaW, bool solo) {
    if (solo) {
        float h = (float)std::max(1, en.maxHp);
        return std::clamp(32.f + h * 0.018f, 34.f, std::min(50.f, arenaW * 0.068f));
    }
    int mh = en.maxHp;
    float r = std::clamp(arenaW * 0.028f, 20.f, 26.f);
    if (mh >= 180)
        r = std::clamp(arenaW * 0.036f, 25.f, 32.f);
    else if (mh >= 110)
        r = std::clamp(arenaW * 0.032f, 22.f, 29.f);
    return r;
}

/* jiggle enemies so sprites arent stacked */
static void relaxEnemyBlobPositions(int nf, float* px, float* py, const float* rad, ArenaLayout* A,
                                    const SharedMemory* s, float left, float right, float stageLineY) {
    const float kPadFoe = 7.f;
    const float kPadHero = 11.f;
    const float kEdge = 8.f;
    const float yLo = stageLineY - 56.f;
    const float yHi = stageLineY + 32.f;
    const int kIter = 24;

    for (int it = 0; it < kIter; it++) {
        for (int i = 0; i < nf; i++) {
            for (int j = i + 1; j < nf; j++) {
                float dx = px[j] - px[i];
                float dy = py[j] - py[i];
                float distSq = dx * dx + dy * dy;
                float minD = rad[i] + rad[j] + kPadFoe;
                if (distSq < 1e-6f) {
                    dx = 0.58f;
                    dy = 0.82f;
                    distSq = dx * dx + dy * dy;
                }
                float dist = std::sqrt(distSq);
                if (dist >= minD) continue;
                float overlap = (minD - dist) * 0.5f;
                float ux = dx / dist, uy = dy / dist;
                px[i] -= ux * overlap;
                py[i] -= uy * overlap;
                px[j] += ux * overlap;
                py[j] += uy * overlap;
            }
        }
        for (int i = 0; i < nf; i++) {
            for (int pi = 0; pi < s->numPlayers; pi++) {
                if (!s->players[pi].isAlive) continue;
                if (A->pCx[pi] < 0.f) continue;
                float dx = px[i] - A->pCx[pi];
                float dy = py[i] - A->pCy[pi];
                float distSq = dx * dx + dy * dy;
                float minD = rad[i] + A->pR[pi] + kPadHero;
                if (distSq < 1e-6f) {
                    dx = 1.f;
                    dy = 0.f;
                    distSq = 1.f;
                }
                float dist = std::sqrt(distSq);
                if (dist >= minD) continue;
                float overlap = minD - dist;
                float ux = dx / dist, uy = dy / dist;
                px[i] += ux * overlap;
                py[i] += uy * overlap;
            }
        }
        for (int i = 0; i < nf; i++) {
            px[i] = std::clamp(px[i], left + rad[i] + kEdge, right - rad[i] - kEdge);
            py[i] = std::clamp(py[i], yLo, yHi);
        }
    }
}

static void layoutArena(const SharedMemory* s, float left, float right, float stageLineY, float focalX,
                        ArenaLayout* A) {
    for (int i = 0; i < cr::maxPlayers; i++) {
        A->pCx[i] = -1.f;
        A->pCy[i] = 0.f;
        A->pR[i] = 0.f;
    }
    for (int i = 0; i < cr::maxEnemies; i++) {
        A->eCx[i] = -1.f;
        A->eCy[i] = 0.f;
        A->eR[i] = 0.f;
    }

    float arenaW = right - left;
    float stance = std::max(80.f, std::min(158.f, arenaW * 0.138f));
    float confront = std::max(18.f, std::min(36.f, arenaW * 0.028f));

    int pAlive = 0;
    for (int i = 0; i < s->numPlayers; i++)
        if (s->players[i].isAlive) pAlive++;
    int eAlive = countAliveEnemies(s);

    float spreadP = std::max(34.f, std::min(44.f, arenaW * 0.056f));
    float rHero = std::max(19.f, std::min(24.f, arenaW * 0.026f));

    int pi = 0;
    for (int i = 0; i < s->numPlayers; i++) {
        if (!s->players[i].isAlive) continue;
        float u = (pAlive <= 1) ? 0.f : ((float)pi - (float)(pAlive - 1) * 0.5f);
        float idlX = std::sin((float)gFrame * 0.034f + (float)i * 1.47f) * 2.0f;
        float idlY = std::cos((float)gFrame * 0.029f + (float)i * 1.63f) * 1.2f;
        A->pCx[i] = focalX - stance - confront * 0.52f + u * spreadP + idlX;
        A->pCy[i] = stageLineY + 6.f + std::fabs(u) * 5.f + idlY;
        A->pR[i] = rHero;
        pi++;
    }

    if (eAlive == 1) {
        for (int slot = 0; slot < cr::maxEnemies; slot++) {
            if (!s->enemies[slot].isAlive) continue;
            float rr = foeBlobRadius(s->enemies[slot], arenaW, true);
            float idlX = std::sin((float)gFrame * 0.038f + (float)slot * 1.6f) * 1.8f;
            float idlY = std::cos((float)gFrame * 0.032f + (float)slot * 1.9f) * 1.1f;
            float ph = (float)slot * 2.17f + 0.33f * (float)s->enemySpawnSeq;
            float scatterX = std::sin(ph) * std::min(arenaW * 0.034f, 44.f);
            float scatterY = std::cos(ph * 1.31f) * 14.f;
            float bx = focalX + stance + confront + std::max(14.f, arenaW * 0.017f) + idlX + scatterX;
            bx = std::clamp(bx, left + rr + 12.f, right - rr - 12.f);
            A->eCx[slot] = bx;
            A->eCy[slot] = stageLineY - 4.f + idlY + scatterY;
            A->eR[slot] = rr;
            break;
        }
        return;
    }

    int slots[cr::maxEnemies];
    float rad[cr::maxEnemies];
    int nf = 0;
    for (int slot = 0; slot < cr::maxEnemies; slot++) {
        if (!s->enemies[slot].isAlive) continue;
        slots[nf] = slot;
        rad[nf] = foeBlobRadius(s->enemies[slot], arenaW, false);
        nf++;
    }
    if (nf == 0) return;

    float gap = std::max(10.f, std::min(22.f, arenaW * 0.02f));
    float cxPos[cr::maxEnemies];
    cxPos[0] = 0.f;
    for (int i = 1; i < nf; i++) cxPos[i] = cxPos[i - 1] + rad[i - 1] + gap + rad[i];

    float spanL = cxPos[0] - rad[0];
    float spanR = cxPos[nf - 1] + rad[nf - 1];
    float spanMid = (spanL + spanR) * 0.5f;
    float depthSpan = std::max(1.f, spanR - spanL);
    float anchor = focalX + stance + confront + std::min(22.f, arenaW * 0.02f);

    float scatterAmpX = std::min(arenaW * 0.045f, 58.f);
    float scatterAmpY = 20.f;

    for (int i = 0; i < nf; i++) {
        int slot = slots[i];
        float worldX = anchor + (cxPos[i] - spanMid);
        float tn = (cxPos[i] - spanMid) / depthSpan;
        float arcY = (1.f - tn * tn) * 5.f;
        float idlX = std::sin((float)gFrame * 0.041f + (float)slot * 1.89f) * 2.5f;
        float idlY = std::cos((float)gFrame * 0.035f + (float)slot * 2.11f) * 1.7f;
        /* Stable per-slot scatter so blobs aren’t one straight line; small depth keeps the formation readable. */
        float ph = (float)slot * 2.513f + (float)i * 0.73f + 0.21f * (float)nf;
        float scatterX =
            std::sin(ph) * scatterAmpX * 0.62f + std::sin(ph * 1.73f + 0.9f) * scatterAmpX * 0.38f;
        float scatterY =
            std::cos(ph * 1.19f) * scatterAmpY + std::sin((float)i * 1.37f + ph) * (scatterAmpY * 0.45f);
        float cx = worldX + idlX + scatterX;
        cx = std::clamp(cx, left + rad[i] + 10.f, right - rad[i] - 10.f);
        A->eCx[slot] = cx;
        A->eCy[slot] = stageLineY - 10.f - std::fabs(tn) * 9.f - arcY + idlY + scatterY;
        A->eR[slot] = rad[i];
    }

    float px[cr::maxEnemies], py[cr::maxEnemies];
    for (int i = 0; i < nf; i++) {
        px[i] = A->eCx[slots[i]];
        py[i] = A->eCy[slots[i]];
    }
    relaxEnemyBlobPositions(nf, px, py, rad, A, s, left, right, stageLineY);
    for (int i = 0; i < nf; i++) {
        int slot = slots[i];
        A->eCx[slot] = px[i];
        A->eCy[slot] = py[i];
    }
}

static void drawBattlefield(sf::RenderTarget& tgt, const sf::Font& font, sf::FloatRect inner, const SharedMemory* s,
                            float shakeX, float shakeY) {
    float padX = (float)(2 * kUiGrid);
    float padY = (float)(2 * kUiGrid);
    float left = inner.left + padX + shakeX;
    float right = inner.left + inner.width - padX + shakeX;
    float top = inner.top + padY - 2.f + shakeY;
    float bot = inner.top + inner.height - padY - 2.f + shakeY;
    if (right - left < 40.f || bot - top < 40.f) return;

    float ih = bot - top;
    float skyH = std::max(46.f, std::min(74.f, ih * 0.21f));
    float horizonY = top + skyH;
    float grassY0 = horizonY + (float)kUiGrid;

    static sf::Texture gBattleMapTex;
    static int gBattleMapLoadState = 0;
    if (gBattleMapLoadState == 0) {
        gBattleMapLoadState = loadBattlefieldMapTexture(gBattleMapTex) ? 2 : 1;
    }
    const bool useBattleMap = (gBattleMapLoadState == 2);

    static sf::Texture gHeroTexP1, gHeroTexP2;
    static sf::Texture gEnemyTexE1;
    static sf::Texture gEnemyTexChar5, gEnemyTexChar7, gEnemyTexChar8;
    static int gHeroTexLoadState = 0;
    static bool gHeroTexP1Ok = false, gHeroTexP2Ok = false;
    static bool gEnemyTexE1Ok = false;
    static bool gEnemyChar5Ok = false, gEnemyChar7Ok = false, gEnemyChar8Ok = false;
    if (gHeroTexLoadState == 0) {
        gHeroTexLoadState = 1;
        gHeroTexP1Ok = loadTextureFromSpritesDir(gHeroTexP1, "char3.png");
        gHeroTexP2Ok = loadTextureFromSpritesDir(gHeroTexP2, "char2.png");
        gEnemyTexE1Ok = loadTextureFromSpritesDir(gEnemyTexE1, "E1.png");
        /* Linux filenames are case-sensitive (CHAR5 vs char7). */
        gEnemyChar5Ok = loadTextureFromSpritesDir(gEnemyTexChar5, "CHAR5.png");
        gEnemyChar7Ok = loadTextureFromSpritesDir(gEnemyTexChar7, "char7.png");
        gEnemyChar8Ok = loadTextureFromSpritesDir(gEnemyTexChar8, "char8.png");
    }

    if (useBattleMap) {
        /* Anchor slightly low: more sky / horizon reads as “stage”, units sit in the field. */
        drawBattlefieldMap(tgt, gBattleMapTex, kBattleMapTexRect, left, top, right, bot, 0.56f);
        sf::RectangleShape veil({right - left, bot - top});
        veil.setPosition(left, top);
        veil.setFillColor(sf::Color(8, 10, 14, 68));
        tgt.draw(veil);
        drawBattleVignette(tgt, left, top, right - left, bot - top);
    } else {
        drawSkyGradient(tgt, left, top, right - left, horizonY);

        sf::RectangleShape horizonGlow({right - left, 6.f});
        horizonGlow.setPosition(left, horizonY - 2.f);
        horizonGlow.setFillColor(sf::Color(62, 92, 54, 90));
        tgt.draw(horizonGlow);
        sf::RectangleShape horizonLine({right - left, 2.f});
        horizonLine.setPosition(left, horizonY + 2.f);
        horizonLine.setFillColor(sf::Color(48, 72, 42));
        tgt.draw(horizonLine);

        float fieldH = bot - grassY0;
        for (int i = 0; i < 5; i++) {
            float t0 = (float)i / 5.f;
            float t1 = (float)(i + 1) / 5.f;
            sf::Uint8 gr = lerpU8(58, 92, t0);
            sf::Uint8 gg = lerpU8(118, 168, t0);
            sf::Uint8 gb = lerpU8(62, 88, t0);
            sf::RectangleShape strip({right - left, (t1 - t0) * fieldH + 0.5f});
            strip.setPosition(left, grassY0 + t0 * fieldH);
            strip.setFillColor(sf::Color(gr, gg, gb));
            tgt.draw(strip);
        }
    }

    float fieldH = bot - grassY0;
    float arenaW = right - left;
    float arenaMidX = (left + right) * 0.5f;
    /* Focal point nudged toward heroes (left): matches PARTY column under battle, clearer duel read. */
    float focalX = arenaMidX - std::clamp(arenaW * 0.032f, 8.f, 26.f);
    float stageLineY = grassY0 + fieldH * 0.525f;
    float spotCx = focalX + arenaW * 0.04f;
    float spotCy = stageLineY + fieldH * 0.04f;
    drawArenaSpotlight(tgt, spotCx, spotCy, std::min(fieldH, arenaW) * 0.44f, std::min(fieldH, arenaW) * 0.15f);

    {
        float slW = std::max(40.f, right - left - (float)(6 * kUiGrid));
        float slX = left + (right - left - slW) * 0.5f;
        sf::RectangleShape stageLn({slW, 1.f});
        stageLn.setPosition(slX, stageLineY + 1.f);
        stageLn.setFillColor(sf::Color(48, 62, 52, 100));
        tgt.draw(stageLn);
    }

    sf::RectangleShape floorShade({right - left, 12.f});
    floorShade.setPosition(left, bot - 11.f);
    floorShade.setFillColor(sf::Color(28, 42, 36, 115));
    tgt.draw(floorShade);

    static const sf::Color kHeroPalette[cr::maxPlayers] = {
        {52, 158, 255},
        {255, 86, 118},
        {255, 214, 72},
        {102, 235, 132},
    };
    static const sf::Color kEnemyBody(235, 58, 82);

    ArenaLayout ar;
    layoutArena(s, left, right, stageLineY, focalX, &ar);
    const float projT = fxProjNormT(s);
    const float kAtkWindup = 0.14f;
    float atkPhase = -1.f;
    if (projT >= 0.f) {
        if (projT <= kAtkWindup)
            atkPhase = 0.f;
        else
            atkPhase = (projT - kAtkWindup) / (1.f - kAtkWindup);
    }
    auto smoothStep = [](float x) {
        x = std::clamp(x, 0.f, 1.f);
        return x * x * (3.f - 2.f * x);
    };

    int pFlashOn =
        (s->hitFlashPlayerIdx >= 0 && monoBeforeEnd(&s->hitFlashPlayerEndMono)) ? s->hitFlashPlayerIdx : -1;
    int eFlashOn =
        (s->hitFlashEnemySlot >= 0 && monoBeforeEnd(&s->hitFlashEnemyEndMono)) ? s->hitFlashEnemySlot : -1;
    int healOn = (s->fxHealPlayerIdx >= 0 && monoBeforeEnd(&s->fxHealEndMono)) ? s->fxHealPlayerIdx : -1;

    int pAlive = 0;
    for (int i = 0; i < s->numPlayers; i++)
        if (s->players[i].isAlive) pAlive++;
    int eAliveTotal = countAliveEnemies(s);

    for (int i = 0; i < s->numPlayers; i++) {
        if (!s->players[i].isAlive) continue;
        if (ar.pCx[i] < 0.f) continue;
        const Entity* pl = &s->players[i];
        float blobCx = ar.pCx[i];
        float blobCy = ar.pCy[i];
        float r = ar.pR[i];
        float lungeX = 0.f;
        if (atkPhase >= 0.f && s->fxProjKind == 1 && s->fxProjPlayerIdx == i) {
            if (atkPhase < 0.48f) {
                float u = atkPhase / 0.48f;
                lungeX = std::sin(3.14159265f * u) * 17.f;
            } else {
                float u = (atkPhase - 0.48f) / 0.52f;
                lungeX = std::sin(3.14159265f * (1.f - u)) * -7.f;
            }
        }
        float hox = 0.f, hoy = 0.f;
        if (i == pFlashOn) {
            sampleDecayShake(&s->hitFlashPlayerEndMono, 6.5f, &hox, &hoy);
            float rem = monoRemainMs(&s->hitFlashPlayerEndMono);
            float age = 1.f - std::clamp(rem / 480.f, 0.f, 1.f);
            hox -= age * age * 11.f;
        }
        blobCx += lungeX + hox;
        blobCy += hoy;
        float squashX = 1.f, squashY = 1.f;
        if (i == pFlashOn) {
            float rem = monoRemainMs(&s->hitFlashPlayerEndMono);
            float age = 1.f - std::clamp(rem / 480.f, 0.f, 1.f);
            float pulse = std::sin(age * 3.14159265f) * 0.11f;
            squashX = 1.f + pulse;
            squashY = 1.f - pulse * 0.5f;
        }
        sf::Color body = kHeroPalette[i % cr::maxPlayers];
        if (i == pFlashOn)
            body = sf::Color(255, 85, 95);
        else if (i == healOn)
            body = sf::Color(120, 255, 170);
        drawUnitShadow(tgt, blobCx, blobCy + r * 0.92f * squashY, r * squashX);
        const sf::Texture* heroSprTex = nullptr;
        if (i == 0 && gHeroTexP1Ok)
            heroSprTex = &gHeroTexP1;
        else if (i == 1 && gHeroTexP2Ok)
            heroSprTex = &gHeroTexP2;
        if (heroSprTex) {
            sf::Sprite spr(*heroSprTex);
            auto sz = heroSprTex->getSize();
            float tw = (float)sz.x, th = (float)sz.y;
            float scale = (2.f * r) / th;
            spr.setScale(scale * squashX, scale * squashY);
            spr.setOrigin(tw * 0.5f, th);
            spr.setPosition(blobCx, blobCy + r * squashY);
            sf::Color tint(255, 255, 255);
            if (i == pFlashOn)
                tint = sf::Color(255, 85, 95);
            else if (i == healOn)
                tint = sf::Color(120, 255, 170);
            spr.setColor(tint);
            tgt.draw(spr);
        } else {
            sf::CircleShape heroBlob(r);
            heroBlob.setOrigin(r, r);
            heroBlob.setPosition(blobCx, blobCy);
            heroBlob.setScale(squashX, squashY);
            heroBlob.setFillColor(body);
            heroBlob.setOutlineThickness(2.f);
            heroBlob.setOutlineColor(sf::Color(16, 20, 26));
            tgt.draw(heroBlob);
            heroBlob.setScale(1.f, 1.f);
            float hiR = r * 0.32f;
            sf::CircleShape hi(hiR);
            hi.setOrigin(hiR, hiR);
            hi.setPosition(blobCx - r * squashX * 0.35f, blobCy - r * squashY * 0.35f);
            hi.setFillColor(sf::Color(255, 255, 255, 100));
            tgt.draw(hi);
            sf::Color wedgeCol(body.r / 2 + 20, body.g / 2 + 20, body.b / 2 + 30, 255);
            drawFacingWedge(tgt, blobCx, blobCy, r * squashX, true, wedgeCol);
        }

        char buf[32];
        if (pAlive > 1)
            snprintf(buf, sizeof(buf), "P%d", i);
        else
            snprintf(buf, sizeof(buf), "P");
        float labelY = blobCy - r - 22.f;
        drawTextShadow(tgt, font, uiSize(14), buf, blobCx - r, labelY, sf::Color(255, 252, 248), sf::Color(8, 10, 14));
        int isActorsTurn = (s->currentActorId == i && i < s->numPlayers);
        if (isActorsTurn)
            drawCornerBrackets(tgt, blobCx - r - 6.f, blobCy - r - 6.f, 2.f * r + 12.f, 2.f * r + 12.f,
                               sf::Color(255, 255, 255, 235), 8.f, 2.f);
        float hpF = pl->maxHp > 0 ? (float)pl->hp / (float)pl->maxHp : 0.f;
        float stF = pl->maxStamina > 0 ? pl->stamina / (float)pl->maxStamina : 0.f;
        float bw = std::clamp(r * 1.36f, 50.f, 68.f);
        float bx = blobCx - bw * 0.5f;
        float by = blobCy + r + 7.f;
        const float agh = 3.f;
        drawThinGauge(tgt, bx, by, bw, agh, hpF, kGaugePartyHpFill, kGaugePartyHpTrack);
        drawThinGauge(tgt, bx, by + agh + 3.f + 2.f, bw, agh, stF, kGaugePartyStFill, kGaugePartyStTrack);
    }

    for (int slot = 0; slot < cr::maxEnemies; slot++) {
        if (!s->enemies[slot].isAlive) continue;
        if (ar.eCx[slot] < 0.f) continue;
        int targetOn = (s->uiTargetEnemySlot == slot && monoBeforeEnd(&s->uiTargetEndMono));
        int blinkOn = ((gFrame / 3) & 1);
        sf::Color body = kEnemyBody;
        if (targetOn && blinkOn)
            body = sf::Color(255, 200, 80);
        else if (slot == eFlashOn)
            body = sf::Color(255, 160, 160);
        float blobCx = ar.eCx[slot];
        float blobCy = ar.eCy[slot];
        float er = ar.eR[slot];
        float elungeX = 0.f;
        if (atkPhase >= 0.f && s->fxProjKind == 2 && s->fxProjEnemySlot == slot) {
            if (atkPhase < 0.48f) {
                float u = atkPhase / 0.48f;
                elungeX = -std::sin(3.14159265f * u) * 15.f;
            } else {
                float u = (atkPhase - 0.48f) / 0.52f;
                elungeX = -std::sin(3.14159265f * (1.f - u)) * -6.f;
            }
        }
        float eox = 0.f, eoy = 0.f;
        if (slot == eFlashOn) {
            sampleDecayShake(&s->hitFlashEnemyEndMono, 7.f, &eox, &eoy);
            float rem = monoRemainMs(&s->hitFlashEnemyEndMono);
            float age = 1.f - std::clamp(rem / 480.f, 0.f, 1.f);
            eox += age * age * 13.f;
        }
        blobCx += elungeX + eox;
        blobCy += eoy;
        float esx = 1.f, esy = 1.f;
        if (slot == eFlashOn) {
            float rem = monoRemainMs(&s->hitFlashEnemyEndMono);
            float age = 1.f - std::clamp(rem / 480.f, 0.f, 1.f);
            float pulse = std::sin(age * 3.14159265f) * 0.13f;
            esx = 1.f + pulse;
            esy = 1.f - pulse * 0.55f;
        }
        drawUnitShadow(tgt, blobCx, blobCy + er * 0.92f * esy, er * esx);
        /* Slot 0: E1.png. Slots 1–8: rotate CHAR5 → char7 → char8; fallbacks if a file is missing. */
        const sf::Texture* enemySprTex = nullptr;
        if (slot == 0) {
            if (gEnemyTexE1Ok)
                enemySprTex = &gEnemyTexE1;
            else if (gEnemyChar5Ok)
                enemySprTex = &gEnemyTexChar5;
        } else {
            int k = (slot - 1) % 3;
            if (k == 0 && gEnemyChar5Ok)
                enemySprTex = &gEnemyTexChar5;
            else if (k == 1 && gEnemyChar7Ok)
                enemySprTex = &gEnemyTexChar7;
            else if (k == 2 && gEnemyChar8Ok)
                enemySprTex = &gEnemyTexChar8;
            if (!enemySprTex && gEnemyTexE1Ok)
                enemySprTex = &gEnemyTexE1;
            if (!enemySprTex && gEnemyChar5Ok)
                enemySprTex = &gEnemyTexChar5;
            if (!enemySprTex && gEnemyChar7Ok)
                enemySprTex = &gEnemyTexChar7;
            if (!enemySprTex && gEnemyChar8Ok)
                enemySprTex = &gEnemyTexChar8;
        }
        if (enemySprTex) {
            sf::Sprite spr(*enemySprTex);
            auto sz = enemySprTex->getSize();
            float tw = (float)sz.x, th = (float)sz.y;
            float scale = (2.f * er) / th;
            spr.setScale(scale * esx, scale * esy);
            spr.setOrigin(tw * 0.5f, th);
            spr.setPosition(blobCx, blobCy + er * esy);
            /* Multiply tint: dark CHAR art barely shifts on (255,160,160); brighten flash so all slots read like E1. */
            sf::Color tint = body;
            if (slot == eFlashOn)
                tint = sf::Color(255, 228, 235);
            spr.setColor(tint);
            tgt.draw(spr);
            if (slot == eFlashOn) {
                float rem = monoRemainMs(&s->hitFlashEnemyEndMono);
                float age = 1.f - std::clamp(rem / 480.f, 0.f, 1.f);
                sf::Uint8 a = (sf::Uint8)std::lround(40.f + 175.f * age * age);
                sf::Sprite hitGlow(spr);
                hitGlow.setColor(sf::Color(255, 200, 210, a));
                sf::RenderStates addGlow;
                addGlow.blendMode = sf::BlendAdd;
                tgt.draw(hitGlow, addGlow);
            }
        } else {
            sf::CircleShape foeBlob(er);
            foeBlob.setOrigin(er, er);
            foeBlob.setPosition(blobCx, blobCy);
            foeBlob.setScale(esx, esy);
            foeBlob.setFillColor(body);
            foeBlob.setOutlineThickness(2.f);
            foeBlob.setOutlineColor(sf::Color(36, 10, 16));
            tgt.draw(foeBlob);
            foeBlob.setScale(1.f, 1.f);
            float eye = er * 0.22f;
            sf::RectangleShape e1({eye, eye});
            e1.setPosition(blobCx - er * esx * 0.55f, blobCy - er * esy * 0.28f);
            e1.setFillColor(sf::Color(22, 18, 28));
            tgt.draw(e1);
            sf::RectangleShape e2({eye, eye});
            e2.setPosition(blobCx + er * esx * 0.28f, blobCy - er * esy * 0.28f);
            e2.setFillColor(sf::Color(22, 18, 28));
            tgt.draw(e2);
            sf::Color wedgeCol(body.r / 2 + 24, body.g / 3 + 10, body.b / 3 + 12, 255);
            drawFacingWedge(tgt, blobCx, blobCy, er * esx, false, wedgeCol);
        }

        char buf[32];
        if (targetOn && blinkOn)
            snprintf(buf, sizeof(buf), ">E%d<", slot);
        else
            snprintf(buf, sizeof(buf), "E%d", slot);
        float labelY = blobCy - er - 24.f;
        if (eAliveTotal == 1)
            drawTextShadow(tgt, font, uiSize(11), "BOSS", blobCx - 24.f, labelY - 16.f, sf::Color(255, 220, 120),
                           sf::Color(48, 28, 8));
        drawTextShadow(tgt, font, uiSize(14), buf, blobCx - er, labelY, sf::Color(255, 245, 248), sf::Color(18, 6, 10));
        const Entity* en = &s->enemies[slot];
        float hpFe = en->maxHp > 0 ? (float)en->hp / (float)en->maxHp : 0.f;
        float stFe = en->maxStamina > 0 ? en->stamina / (float)en->maxStamina : 0.f;
        float bw = std::clamp(er * 1.34f, 52.f, 72.f);
        float bx = blobCx - bw * 0.5f;
        float by = blobCy + er * esy + 7.f;
        const float fgh = 3.f;
        const float fStack = fgh + 3.f + 2.f;
        drawThinGauge(tgt, bx, by, bw, fgh, hpFe, kGaugeFoeHpFill, kGaugeFoeHpTrack);
        drawThinGauge(tgt, bx, by + fStack, bw, fgh, stFe, kGaugeFoeStFill, kGaugeFoeStTrack);
    }

    if (projT >= 0.f) {
        float t = projT;
        float moveT = (atkPhase >= 0.f) ? smoothStep(atkPhase) : t;
        float py = stageLineY - 26.f;
        if (py < grassY0) py = grassY0 + 8.f;
        if (s->fxProjKind == 1) {
            float pc = ar.pCx[s->fxProjPlayerIdx];
            float ec = ar.eCx[s->fxProjEnemySlot];
            float pcy = ar.pCy[s->fxProjPlayerIdx];
            float ecy = ar.eCy[s->fxProjEnemySlot];
            if (pc >= 0.f && ec >= 0.f) {
                py = (pcy + ecy) * 0.5f - 18.f;
                if (t > 0.20f && t < 0.58f) {
                    float u = (t - 0.20f) / 0.38f;
                    float ang = (float)std::atan2(ecy - pcy, ec - pc) - (1.f - u) * 0.85f;
                    float len = std::clamp(std::hypot(ec - pc, ecy - pcy) * 0.55f, 18.f, 56.f);
                    sf::Vertex slash[2];
                    slash[0].position = sf::Vector2f(pc + std::cos(ang) * 8.f, pcy + std::sin(ang) * 8.f);
                    slash[1].position = sf::Vector2f(pc + std::cos(ang) * len, pcy + std::sin(ang) * len);
                    slash[0].color = sf::Color(255, 252, 235, 230);
                    slash[1].color = sf::Color(200, 235, 255, 40);
                    tgt.draw(slash, 2, sf::Lines);
                }
                float x = pc + moveT * (ec - pc);
                sf::CircleShape core(5.f);
                core.setOrigin(5.f, 5.f);
                core.setPosition(x, py);
                core.setFillColor(sf::Color(120, 235, 255));
                tgt.draw(core);
                for (int k = 1; k <= 4; k++) {
                    float tx = pc + (moveT - 0.08f * (float)k) * (ec - pc);
                    if (tx >= left && tx <= right && moveT > 0.08f * (float)k) {
                        sf::CircleShape tr(2.5f);
                        tr.setOrigin(2.5f, 2.5f);
                        tr.setPosition(tx, py);
                        tr.setFillColor(sf::Color(180, 240, 255, 170));
                        tgt.draw(tr);
                    }
                }
            }
        } else if (s->fxProjKind == 2) {
            float ec = ar.eCx[s->fxProjEnemySlot];
            float pc = ar.pCx[s->fxProjPlayerIdx];
            float pcy = ar.pCy[s->fxProjPlayerIdx];
            float ecy = ar.eCy[s->fxProjEnemySlot];
            if (pc >= 0.f && ec >= 0.f) {
                py = (pcy + ecy) * 0.5f - 18.f;
                if (t > 0.20f && t < 0.58f) {
                    float u = (t - 0.20f) / 0.38f;
                    float ang = (float)std::atan2(pcy - ecy, pc - ec) - (1.f - u) * 0.75f;
                    float len = std::clamp(std::hypot(pc - ec, pcy - ecy) * 0.48f, 16.f, 50.f);
                    sf::Vertex slash[2];
                    slash[0].position = sf::Vector2f(ec + std::cos(ang) * 7.f, ecy + std::sin(ang) * 7.f);
                    slash[1].position = sf::Vector2f(ec + std::cos(ang) * len, ecy + std::sin(ang) * len);
                    slash[0].color = sf::Color(255, 235, 190, 228);
                    slash[1].color = sf::Color(255, 160, 90, 35);
                    tgt.draw(slash, 2, sf::Lines);
                }
                float x = ec + moveT * (pc - ec);
                sf::CircleShape core(5.f);
                core.setOrigin(5.f, 5.f);
                core.setPosition(x, py);
                core.setFillColor(sf::Color(255, 200, 90));
                tgt.draw(core);
                for (int k = 1; k <= 4; k++) {
                    float tx = ec + (moveT - 0.08f * (float)k) * (pc - ec);
                    if (tx >= left && tx <= right && moveT > 0.08f * (float)k) {
                        sf::CircleShape tr(2.5f);
                        tr.setOrigin(2.5f, 2.5f);
                        tr.setPosition(tx, py);
                        tr.setFillColor(sf::Color(255, 230, 170, 160));
                        tgt.draw(tr);
                    }
                }
            }
        }
    }

    if (s->dmgPopupKind != 0 && monoBeforeEnd(&s->dmgPopupEndMono)) {
        double a = monoAsDouble(&s->dmgPopupStartMono);
        double b = monoAsDouble(&s->dmgPopupEndMono);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double tn = monoAsDouble(&now);
        double t = (b > a) ? (tn - a) / (b - a) : 1.0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float dy = (float)(t * 18.0);
        float px = -1.f;
        float pyBase = stageLineY;
        if (s->dmgPopupKind == 1 && s->dmgPopupEnemySlot >= 0 && s->dmgPopupEnemySlot < cr::maxEnemies) {
            px = ar.eCx[s->dmgPopupEnemySlot];
            pyBase = ar.eCy[s->dmgPopupEnemySlot];
        }
        if (s->dmgPopupKind == 2 && s->dmgPopupPlayerIdx >= 0 && s->dmgPopupPlayerIdx < cr::maxPlayers) {
            px = ar.pCx[s->dmgPopupPlayerIdx];
            pyBase = ar.pCy[s->dmgPopupPlayerIdx];
        }
        /* eCx unset stays -1 for defeated foes; any laid-out unit has px >= 0. */
        if (px >= 0.f) {
            char buf[32];
            snprintf(buf, sizeof(buf), "-%d", s->dmgPopupValue);
            float py2 = pyBase - 42.f - dy;
            if (py2 < top + 8.f) py2 = top + 8.f;
            float pop = 1.f;
            if (t < 0.14) pop = 0.82f + 0.22f * std::sin((float)(t / 0.14 * 3.14159265 * 0.5));
            unsigned dsz = (unsigned)std::max(10u, (unsigned)std::lround((float)uiSize(16) * pop));
            drawTextShadow(tgt, font, dsz, buf, px - 12.f, py2, sf::Color(255, 130, 130), sf::Color(40, 8, 8));
        }
    }
}

/* ---- Loot modal helpers (dark RPG / CRT): grain, glow, tiered keybind text ------------------- */

static sf::Color lootMulAlpha(sf::Color c, float m) {
    if (m <= 0.f) return sf::Color(c.r, c.g, c.b, 0);
    if (m >= 1.f) return c;
    return sf::Color(c.r, c.g, c.b, (uint8_t)std::lround((float)c.a * m));
}

static uint32_t lootHash2i(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void drawLootGrain(sf::RenderTarget& tgt, sf::FloatRect card, float fadeM) {
    const float step = 5.f;
    sf::VertexArray va(sf::Quads);
    float x0 = card.left;
    float y0 = card.top;
    float x1 = card.left + card.width;
    float y1 = card.top + card.height;
    for (float y = y0; y < y1; y += step) {
        for (float x = x0; x < x1; x += step) {
            uint32_t h = lootHash2i((int)x, (int)y);
            uint8_t n = (uint8_t)(10 + (h % 22));
            sf::Color co = lootMulAlpha(sf::Color(210, 190, 160, n), fadeM);
            float w = std::min(step, x1 - x);
            float hgt = std::min(step, y1 - y);
            va.append(sf::Vertex(sf::Vector2f(x, y), co));
            va.append(sf::Vertex(sf::Vector2f(x + w, y), co));
            va.append(sf::Vertex(sf::Vector2f(x + w, y + hgt), co));
            va.append(sf::Vertex(sf::Vector2f(x, y + hgt), co));
        }
    }
    tgt.draw(va);
}

static void drawLootScanlines(sf::RenderTarget& tgt, sf::FloatRect card, float fadeM) {
    float x = card.left;
    float y0 = card.top;
    float w = card.width;
    float h = card.height;
    int row = 0;
    for (float yy = 0.f; yy < h; yy += 3.f) {
        sf::RectangleShape ln({w, 1.f});
        ln.setPosition(x, y0 + yy);
        uint8_t a = (uint8_t)((row % 3 == 0) ? 26 : 14);
        ln.setFillColor(lootMulAlpha(sf::Color(0, 0, 0, a), fadeM));
        tgt.draw(ln);
        row++;
    }
}

/* sparkle doodad next to loot names */
static void drawLootSparkSymbol(sf::RenderTarget& tgt, float cx, float cy, float fadeM, float pulse) {
    float g = 11.f + 3.f * pulse;
    sf::CircleShape halo(g);
    halo.setOrigin(g, g);
    halo.setPosition(cx, cy);
    halo.setFillColor(lootMulAlpha(sf::Color(255, 185, 55, 100), fadeM));
    tgt.draw(halo);
    sf::Color rayCol = lootMulAlpha(sf::Color(255, 228, 140), fadeM);
    sf::RectangleShape v({3.f, 18.f});
    v.setOrigin(1.5f, 9.f);
    v.setPosition(cx, cy);
    v.setFillColor(rayCol);
    tgt.draw(v);
    sf::RectangleShape h({18.f, 3.f});
    h.setOrigin(9.f, 1.5f);
    h.setPosition(cx, cy);
    h.setFillColor(rayCol);
    tgt.draw(h);
    sf::CircleShape core(3.f);
    core.setOrigin(3.f, 3.f);
    core.setPosition(cx, cy);
    core.setFillColor(lootMulAlpha(sf::Color(255, 252, 235), fadeM));
    tgt.draw(core);
}

static void drawLootRadialNameGlow(sf::RenderTarget& tgt, float cx, float cy, float fadeM, float breathe) {
    const float phases[] = {1.f, 0.72f, 0.48f, 0.28f, 0.14f};
    for (int i = 0; i < 5; i++) {
        float r = 26.f + 48.f * phases[i] * breathe;
        sf::CircleShape g(r);
        g.setOrigin(r, r);
        g.setPosition(cx, cy);
        uint8_t a = (uint8_t)std::lround((16.f + (float)i * 10.f) * breathe);
        g.setFillColor(lootMulAlpha(sf::Color(255, 170, 48, std::min(110, (int)a)), fadeM));
        tgt.draw(g);
    }
}

static void drawLootPulsingEclipseBorder(sf::RenderTarget& tgt, sf::FloatRect card, float fadeM, float pulse) {
    float glow = 0.55f + 0.45f * pulse;
    struct Layer {
        float dx;
        uint8_t baseA;
        sf::Color co;
    };
    const Layer layers[] = {
        {6.f, 26, sf::Color(60, 40, 120)},
        {4.f, 48, sf::Color(120, 90, 200)},
        {2.f, 88, sf::Color(160, 220, 255)},
        {0.f, 155, sf::Color(220, 250, 255)},
    };
    for (const Layer& L : layers) {
        float e = L.dx * 2.f;
        sf::RectangleShape frame({card.width + e, card.height + e});
        frame.setPosition(card.left - L.dx, card.top - L.dx);
        frame.setFillColor(sf::Color::Transparent);
        uint8_t oa = (uint8_t)std::lround((float)L.baseA * glow * fadeM);
        frame.setOutlineColor(sf::Color(L.co.r, L.co.g, L.co.b, std::min(255, (int)oa)));
        frame.setOutlineThickness(2.f);
        tgt.draw(frame);
    }
}

static void drawEclipseRelicOrbIcon(sf::RenderTarget& tgt, float ix, float iy, float fadeM, float pulse) {
    float g = 0.62f + 0.38f * pulse;
    for (int i = 4; i >= 0; i--) {
        float rr = 12.f + (float)i * 9.f;
        sf::CircleShape c(rr);
        c.setOrigin(rr, rr);
        c.setPosition(ix, iy + 10.f);
        uint8_t a = (uint8_t)(8 + i * 16);
        c.setFillColor(lootMulAlpha(sf::Color(100, 60, 200, std::min(130, (int)a)), fadeM * g));
        tgt.draw(c);
    }
    sf::CircleShape core(18.f);
    core.setOrigin(18.f, 18.f);
    core.setPosition(ix, iy + 10.f);
    core.setFillColor(lootMulAlpha(sf::Color(210, 235, 255), fadeM));
    core.setOutlineColor(lootMulAlpha(sf::Color(90, 200, 255), fadeM));
    core.setOutlineThickness(2.f);
    tgt.draw(core);
    sf::ConvexShape crescent(3);
    crescent.setPoint(0, {-5.f, -8.f});
    crescent.setPoint(1, {10.f, 2.f});
    crescent.setPoint(2, {-4.f, 10.f});
    crescent.setFillColor(lootMulAlpha(sf::Color(40, 25, 70, 200), fadeM));
    crescent.setPosition(ix + 4.f, iy + 6.f);
    tgt.draw(crescent);
}

static void drawLootPulsingGoldBorder(sf::RenderTarget& tgt, sf::FloatRect card, float fadeM, float pulse) {
    float glow = 0.55f + 0.45f * pulse;
    struct Layer {
        float dx;
        uint8_t baseA;
        sf::Color co;
    };
    const Layer layers[] = {
        {6.f, 28, sf::Color(180, 110, 20)},
        {4.f, 52, sf::Color(230, 170, 60)},
        {2.f, 95, sf::Color(245, 197, 66)},
        {0.f, 168, sf::Color(255, 225, 140)},
    };
    for (const Layer& L : layers) {
        float e = L.dx * 2.f;
        sf::RectangleShape frame({card.width + e, card.height + e});
        frame.setPosition(card.left - L.dx, card.top - L.dx);
        frame.setFillColor(sf::Color::Transparent);
        uint8_t oa = (uint8_t)std::lround((float)L.baseA * glow * fadeM);
        frame.setOutlineColor(sf::Color(L.co.r, L.co.g, L.co.b, std::min(255, (int)oa)));
        frame.setOutlineThickness(2.f);
        tgt.draw(frame);
    }
}

static void drawLootWeaponIcon(sf::RenderTarget& tgt, float ix, float iy, float fadeM, float pulse) {
    float g = 0.6f + 0.4f * pulse;
    for (int i = 3; i >= 1; i--) {
        float rr = 22.f + (float)i * 8.f;
        sf::CircleShape halo(rr);
        halo.setOrigin(rr, rr);
        halo.setPosition(ix, iy + 18.f);
        halo.setFillColor(lootMulAlpha(sf::Color(255, 160, 40, (uint8_t)(12 + i * 14)), fadeM * g));
        tgt.draw(halo);
    }
    sf::ConvexShape blade(4);
    blade.setPoint(0, {0.f, -26.f});
    blade.setPoint(1, {7.f, 18.f});
    blade.setPoint(2, {0.f, 22.f});
    blade.setPoint(3, {-7.f, 18.f});
    blade.setFillColor(lootMulAlpha(sf::Color(210, 215, 225), fadeM));
    blade.setOutlineColor(lootMulAlpha(sf::Color(60, 62, 72), fadeM));
    blade.setOutlineThickness(1.f);
    blade.setPosition(ix, iy);
    tgt.draw(blade);
    sf::RectangleShape guard({22.f, 4.f});
    guard.setOrigin(11.f, 2.f);
    guard.setPosition(ix, iy + 18.f);
    guard.setFillColor(lootMulAlpha(sf::Color(140, 95, 42), fadeM));
    tgt.draw(guard);
    sf::RectangleShape grip({6.f, 14.f});
    grip.setOrigin(3.f, 0.f);
    grip.setPosition(ix, iy + 20.f);
    grip.setFillColor(lootMulAlpha(sf::Color(70, 52, 38), fadeM));
    tgt.draw(grip);
}

static float lootAsciiGlyphAdvance(const sf::Font& font, unsigned sz, char ch, sf::Uint32 style) {
    char buf[2] = {ch, '\0'};
    sf::Text t;
    t.setFont(font);
    t.setString(buf);
    t.setCharacterSize(sz);
    t.setStyle(style);
    return t.findCharacterPos(1).x;
}

static void drawLootAsciiLetterSpaced(sf::RenderTarget& tgt, const sf::Font& font, unsigned sz, float x, float y,
                                      float tracking, const char* ascii, sf::Color fill, sf::Uint32 style,
                                      sf::Color glowC, float fadeM) {
    float cx = x;
    for (const char* p = ascii; *p; ++p) {
        char buf[2] = {*p, '\0'};
        sf::Text glow;
        glow.setFont(font);
        glow.setString(buf);
        glow.setCharacterSize(sz);
        glow.setStyle(style);
        glow.setFillColor(lootMulAlpha(glowC, fadeM * 0.85f));
        glow.setPosition(cx + 2.f, y + 2.f);
        tgt.draw(glow);
        sf::Text fg;
        fg.setFont(font);
        fg.setString(buf);
        fg.setCharacterSize(sz);
        fg.setStyle(style);
        fg.setFillColor(lootMulAlpha(fill, fadeM));
        fg.setPosition(cx, y);
        tgt.draw(fg);
        cx += lootAsciiGlyphAdvance(font, sz, *p, style) + tracking;
    }
}

static void drawLootItalicLine(sf::RenderTarget& tgt, const sf::Font& font, unsigned sz, float x, float y,
                               const char* utf8, sf::Color fill, float fadeM) {
    sf::Text t;
    t.setFont(font);
    t.setString(utf8);
    t.setCharacterSize(sz);
    t.setStyle(sf::Text::Italic);
    t.setFillColor(lootMulAlpha(fill, fadeM));
    t.setPosition(x, y);
    tgt.draw(t);
}

static void drawLootWeaponNameFX(sf::RenderTarget& tgt, const sf::Font& font, unsigned sz, float x, float y,
                                 const char* name, float fadeM, int age, int frame) {
    float flicker = 1.f;
    if (age < 42) {
        float u = (float)age / 42.f;
        float noise = 0.62f + 0.38f * std::sin((float)age * 1.05f + std::sin((float)age * 0.31f) * 2.f);
        flicker = noise * (1.f - u) + u;
    }
    float shim = 1.f + 0.035f * std::sin((float)frame * 0.092f);
    uint8_t whiteA = (uint8_t)std::lround(252.f * flicker * shim * fadeM);

    sf::Text shadow;
    shadow.setFont(font);
    shadow.setString(name);
    shadow.setCharacterSize(sz);
    shadow.setFillColor(lootMulAlpha(sf::Color(10, 8, 14), fadeM));
    shadow.setPosition(x + 3.f, y + 3.f);
    tgt.draw(shadow);

    sf::Text hi;
    hi.setFont(font);
    hi.setString(name);
    hi.setCharacterSize(sz);
    hi.setFillColor(sf::Color(255, 255, 255, whiteA));
    hi.setPosition(x, y);
    tgt.draw(hi);
}

static void drawLootKeybindTiered(sf::RenderTarget& tgt, const sf::Font& font, unsigned sz, float x, float y,
                                  const char* line, float fadeM) {
    const sf::Color colOut(88, 86, 84);
    const sf::Color colIn(135, 132, 128);
    const sf::Color colBr(196, 192, 186);

    float cx = x;
    bool inside = false;
    for (const char* p = line; *p; ++p) {
        char c = *p;
        sf::Color use = colOut;
        if (c == '[' || c == ']')
            use = colBr;
        else if (inside)
            use = colIn;
        char buf[2] = {c, '\0'};
        sf::Text t;
        t.setFont(font);
        t.setString(buf);
        t.setCharacterSize(sz);
        t.setFillColor(lootMulAlpha(use, fadeM));
        t.setPosition(cx, y);
        tgt.draw(t);
        cx += lootAsciiGlyphAdvance(font, sz, c, 0);
        if (c == '[') inside = true;
        else if (c == ']')
            inside = false;
    }
}

/* red ribbon when something dies */
static void drawEnemyKillBannerOnTop(sf::RenderWindow& window, const sf::Font& font, float W, const SharedMemory* s) {
    if (s->enemyKillBannerEndMono.tv_sec == 0 && s->enemyKillBannerEndMono.tv_nsec == 0) return;
    if (!monoBeforeEnd(&s->enemyKillBannerEndMono)) return;

    float rem = monoRemainMs(&s->enemyKillBannerEndMono);
    float fade = std::clamp(rem / 1200.f, 0.25f, 1.f);
    uint8_t a = (uint8_t)std::lround(215.f * fade);

    sf::RectangleShape bar({W - 36.f, 62.f});
    bar.setPosition(18.f, 14.f);
    bar.setFillColor(sf::Color(140, 28, 18, a));
    bar.setOutlineColor(sf::Color(255, 210, 120, (uint8_t)std::lround(180.f * fade)));
    bar.setOutlineThickness(2.f);
    window.draw(bar);

    drawTextShadow(window, font, uiSize(26), "ENEMY DEFEATED", 34.f, 28.f, sf::Color(255, 250, 220),
                   sf::Color(40, 10, 6));
    drawText(window, font, uiSize(12), "Kill registered — see loot prompt below when a weapon drops.", 34.f, 56.f,
             sf::Color(255, 220, 190));
}

/* big red YOU CANT when ultimate fails */
static void drawUltimateDeniedOverlayOnTop(sf::RenderWindow& window, const sf::Font& font, float W, float H,
                                           const SharedMemory* s) {
    if (!monoBeforeEnd(&s->ultimateDeniedEndMono)) return;

    struct timespec nowTs;
    clock_gettime(CLOCK_MONOTONIC, &nowTs);
    double n = monoAsDouble(&nowTs);
    double a = monoAsDouble(&s->ultimateDeniedStartMono);
    double b = monoAsDouble(&s->ultimateDeniedEndMono);
    double dur = b - a;
    if (dur <= 0) return;
    float t = (float)((n - a) / dur);
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;

    float envIn = std::clamp(t / 0.13f, 0.f, 1.f);
    float envOut = std::clamp((1.f - t) / 0.13f, 0.f, 1.f);
    float envelope = std::min(envIn, envOut);

    uint8_t scrimA = (uint8_t)std::lround(132.f * envelope);
    sf::RectangleShape scrim({W, H});
    scrim.setPosition(0.f, 0.f);
    scrim.setFillColor(sf::Color(4, 6, 10, scrimA));
    window.draw(scrim);

    float enter = std::clamp(t / 0.17f, 0.f, 1.f);
    float easeEnter = 1.f - std::pow(1.f - enter, 2.35f);
    float ySlide = (1.f - easeEnter) * 32.f;

    unsigned titleSz = (unsigned)std::max(30u, (unsigned)std::lround(46.f * gUiScale));
    const char* title = "ULTIMATE DENIED";
    float tw = measureTextWidth(font, titleSz, title);
    float cx = std::floor(W * 0.5f - tw * 0.5f);
    float cy = std::floor(H * 0.48f - (float)titleSz * 0.45f + ySlide);

    uint8_t ta = (uint8_t)std::lround(255.f * envelope);
    sf::Color fill(248, 38, 56, ta);
    sf::Color shadow(26, 6, 10, ta);
    drawTextShadow(window, font, titleSz, title, cx, cy, fill, shadow);
}

/* weapon hit the floor — fancy loot card */
static void drawWeaponDropPopupOnTop(sf::RenderWindow& window, const sf::Font& font, float W, float H,
                                     const SharedMemory* s) {
    static bool s_lootAnimPrimed = false;
    static int s_lootAnim0 = 0;

    if (!s->dropPending || s->dropDecided) {
        s_lootAnimPrimed = false;
        return;
    }
    if (!s_lootAnimPrimed) {
        s_lootAnim0 = gFrame;
        s_lootAnimPrimed = true;
    }
    const int age = gFrame - s_lootAnim0;

    int dw = s->dropWeapon;
    const char* wname = (dw >= 0 && dw < weaponIds::count) ? weaponTable[dw].name : "?";

    const float fadeM = std::min(1.f, (float)age / 16.f);
    const float ease = 1.f - std::pow(1.f - std::min(1.f, (float)age / 22.f), 2.5f);
    const float slide = (1.f - ease) * 44.f;
    const float pulse = 0.5f + 0.5f * std::sin((float)gFrame * 0.051f);
    const float breathe = 0.88f + 0.12f * std::sin((float)gFrame * 0.037f);

    const uint8_t scrimA = (uint8_t)std::lround(188.f * fadeM);
    sf::RectangleShape scrim({W, H});
    scrim.setPosition(0.f, 0.f);
    scrim.setFillColor(sf::Color(0, 0, 0, scrimA));
    window.draw(scrim);

    const float pad = 12.f;
    const float iconCol = 54.f;
    const float bw = std::floor(std::min(W * 0.68f, 560.f));
    const float bh = std::floor(std::clamp(222.f * gUiScale, 204.f, 262.f));
    const float cx = W * 0.5f;
    const float cy = H * 0.40f;
    const float bx = std::floor(cx - bw * 0.5f);
    const float by = std::floor(cy - bh * 0.5f - slide);
    const sf::FloatRect card(bx, by, bw, bh);

    sf::RectangleShape panel({bw, bh});
    panel.setPosition(bx, by);
    panel.setFillColor(lootMulAlpha(sf::Color(26, 15, 0), fadeM));
    window.draw(panel);

    drawLootGrain(window, card, fadeM);
    drawLootPulsingGoldBorder(window, card, fadeM, pulse);

    const float ix = bx + pad + iconCol * 0.48f;
    drawLootWeaponIcon(window, ix, by + 36.f, fadeM, pulse);

    const unsigned szHead = (unsigned)std::max(16u, (unsigned)std::lround(19.f * gUiScale));
    const unsigned szSub = (unsigned)std::max(10u, (unsigned)std::lround(11.f * gUiScale));
    const unsigned szName = (unsigned)std::max(20u, (unsigned)std::lround(27.f * gUiScale));
    const unsigned szHint = (unsigned)std::max(10u, (unsigned)std::lround(11.f * gUiScale));
    const unsigned szFoot = (unsigned)std::max(7u, (unsigned)std::lround(8.f * gUiScale));

    const float xText = bx + pad + iconCol;
    float y = by + pad;

    const float track = std::max(2.f, 4.f * gUiScale);
    drawLootAsciiLetterSpaced(window, font, szHead, xText, y, track, "WEAPON DROPPED",
                              sf::Color(245, 197, 66), sf::Text::Bold, sf::Color(85, 42, 6), fadeM);
    y += (float)szHead + 8.f;

    /* UTF-8 em dash (avoid placeholder tofu from missing glyphs in footer fonts). */
    const char* sub = "Enemy defeated - loot on the ground";
    drawLootItalicLine(window, font, szSub, xText, y, sub, sf::Color(165, 118, 62), fadeM);
    y += (float)szSub + 14.f;

    sf::RectangleShape div({bw - pad * 2.f - iconCol, 1.f});
    div.setPosition(xText, y);
    div.setFillColor(lootMulAlpha(sf::Color(212, 162, 68), fadeM));
    window.draw(div);
    y += 12.f;

    const float sparkCx = xText + 10.f;
    const float sparkCy = y + (float)szName * 0.42f;
    drawLootSparkSymbol(window, sparkCx, sparkCy, fadeM, pulse);
    const float sparkColumnW = 28.f;
    const float nameX = xText + sparkColumnW + 6.f;

    sf::Text measureName;
    measureName.setFont(font);
    measureName.setString(wname);
    measureName.setCharacterSize(szName);
    float nameW = measureName.getLocalBounds().width;
    float nameCx = nameX + nameW * 0.5f;
    float nameCy = y + (float)szName * 0.38f;
    drawLootRadialNameGlow(window, nameCx, nameCy, fadeM, breathe);

    drawLootWeaponNameFX(window, font, szName, nameX, y, wname, fadeM, age, gFrame);
    y += (float)szName + 18.f;

    const int ca = s->currentActorId;
    const bool heroTurn = (ca >= 0 && ca < s->numPlayers && s->players[ca].isAlive);

    if (heroTurn) {
        drawLootKeybindTiered(window, font, szHint, xText, y,
                              "[ Y / E ] Pick up    [ N / S ] Leave it    Enter / Space / Z = pick up", fadeM);
        y += (float)szHint + 14.f;
        drawText(window, font, szFoot, "Legacy keys y / n still work.", xText, y,
                 lootMulAlpha(sf::Color(92, 88, 84), fadeM));
        y += (float)szFoot + 7.f;
        drawText(window, font, szFoot, "If you skip, a living enemy may claim this weapon.", xText, y,
                 lootMulAlpha(sf::Color(74, 70, 66), fadeM));
    } else {
        drawLootKeybindTiered(window, font, szHint, xText, y,
                              "Wait: [ Y / E ] take    [ N / S ] skip when your hero commands.", fadeM);
        y += (float)szHint + 14.f;
        drawText(window, font, szFoot, "Legacy keys y / n still work.", xText, y,
                 lootMulAlpha(sf::Color(92, 88, 84), fadeM));
        y += (float)szFoot + 7.f;
        drawText(window, font, szFoot, "If you skip, a living enemy may claim this weapon.", xText, y,
                 lootMulAlpha(sf::Color(74, 70, 66), fadeM));
    }

    drawLootScanlines(window, card, fadeM);
}

/* same vibe as weapon drop modal but purple/teal */
static void drawEclipseRelicPickupPopupOnTop(sf::RenderWindow& window, const sf::Font& font, float W, float H) {
    static bool s_eclipseAnimPrimed = false;
    static int s_eclipseAnim0 = 0;

    if (!eclipsePickupModalIsVisible()) {
        s_eclipseAnimPrimed = false;
        return;
    }
    if (!s_eclipseAnimPrimed) {
        s_eclipseAnim0 = gFrame;
        s_eclipseAnimPrimed = true;
    }
    const int age = gFrame - s_eclipseAnim0;
    const char* wname = weaponTable[weaponIds::eclipseRelic].name;
    const int wDmg = weaponTable[weaponIds::eclipseRelic].damage;
    const int wSlot = weaponTable[weaponIds::eclipseRelic].slotSize;

    const float fadeM = std::min(1.f, (float)age / 16.f);
    const float ease = 1.f - std::pow(1.f - std::min(1.f, (float)age / 22.f), 2.5f);
    const float slide = (1.f - ease) * 44.f;
    const float pulse = 0.5f + 0.5f * std::sin((float)gFrame * 0.049f);
    const float breathe = 0.88f + 0.12f * std::sin((float)gFrame * 0.035f);

    const uint8_t scrimA = (uint8_t)std::lround(198.f * fadeM);
    sf::RectangleShape scrim({W, H});
    scrim.setPosition(0.f, 0.f);
    scrim.setFillColor(sf::Color(8, 4, 18, scrimA));
    window.draw(scrim);

    const float pad = 12.f;
    const float iconCol = 54.f;
    const float bw = std::floor(std::min(W * 0.72f, 620.f));
    const float bh = std::floor(std::clamp(248.f * gUiScale, 228.f, 288.f));
    const float cx = W * 0.5f;
    const float cy = H * 0.40f;
    const float bx = std::floor(cx - bw * 0.5f);
    const float by = std::floor(cy - bh * 0.5f - slide);
    const sf::FloatRect card(bx, by, bw, bh);

    sf::RectangleShape panel({bw, bh});
    panel.setPosition(bx, by);
    panel.setFillColor(lootMulAlpha(sf::Color(42, 24, 68), fadeM));
    window.draw(panel);

    drawLootGrain(window, card, fadeM);
    drawLootPulsingEclipseBorder(window, card, fadeM, pulse);

    const float ix = bx + pad + iconCol * 0.48f;
    drawEclipseRelicOrbIcon(window, ix, by + 32.f, fadeM, pulse);

    const unsigned szHead = (unsigned)std::max(16u, (unsigned)std::lround(19.f * gUiScale));
    const unsigned szSub = (unsigned)std::max(10u, (unsigned)std::lround(11.f * gUiScale));
    const unsigned szName = (unsigned)std::max(20u, (unsigned)std::lround(27.f * gUiScale));
    const unsigned szHint = (unsigned)std::max(10u, (unsigned)std::lround(11.f * gUiScale));
    const unsigned szFoot = (unsigned)std::max(7u, (unsigned)std::lround(8.f * gUiScale));
    const unsigned szBtn = (unsigned)std::max(11u, (unsigned)std::lround(12.f * gUiScale));

    const float xText = bx + pad + iconCol;
    float y = by + pad;
    const float track = std::max(2.f, 4.f * gUiScale);

    drawLootAsciiLetterSpaced(window, font, szHead, xText, y, track, "ECLIPSE RELIC",
                              sf::Color(200, 240, 255), sf::Text::Bold, sf::Color(60, 30, 110), fadeM);
    y += (float)szHead + 8.f;
    drawLootItalicLine(window, font, szSub, xText, y, "A forbidden artifact manifests on the battlefield",
                      sf::Color(170, 150, 230), fadeM);
    y += (float)szSub + 14.f;

    sf::RectangleShape div({bw - pad * 2.f - iconCol, 1.f});
    div.setPosition(xText, y);
    div.setFillColor(lootMulAlpha(sf::Color(130, 200, 255), fadeM));
    window.draw(div);
    y += 12.f;

    drawLootSparkSymbol(window, xText + 10.f, y + (float)szName * 0.42f, fadeM, pulse);
    const float sparkColumnW = 28.f;
    const float nameX = xText + sparkColumnW + 6.f;
    drawLootRadialNameGlow(window, nameX + 80.f, y + (float)szName * 0.38f, fadeM, breathe);
    drawLootWeaponNameFX(window, font, szName, nameX, y, wname, fadeM, age, gFrame);
    y += (float)szName + 10.f;

    char statLine[96];
    snprintf(statLine, sizeof(statLine), "artifact / %d dmg / needs %d slots — use it like any weapon",
             wDmg, wSlot);
    drawText(window, font, szHint, statLine, xText, y, lootMulAlpha(sf::Color(190, 210, 255), fadeM));
    y += (float)szHint + 18.f;

    const float gapBtn = 14.f;
    const float btnW = (bw - pad * 2.f - iconCol - gapBtn) * 0.5f;
    const float btnH = 44.f;
    const float btnY = y;
    auto drawChoiceBtn = [&](float bx0, const char* title, const char* keys, bool left) {
        sf::RectangleShape b({btnW, btnH});
        b.setPosition(bx0, btnY);
        b.setFillColor(lootMulAlpha(left ? sf::Color(30, 72, 58) : sf::Color(52, 36, 72), fadeM));
        b.setOutlineColor(lootMulAlpha(left ? sf::Color(120, 255, 190) : sf::Color(200, 140, 255), fadeM * 0.92f));
        b.setOutlineThickness(2.f);
        window.draw(b);
        drawText(window, font, szBtn, title, bx0 + 12.f, btnY + 6.f,
                 lootMulAlpha(sf::Color(240, 255, 250), fadeM));
        drawText(window, font, szFoot, keys, bx0 + 12.f, btnY + 26.f,
                 lootMulAlpha(sf::Color(200, 210, 230), fadeM));
    };
    drawChoiceBtn(xText, "YES — pick up", "[ Y ]  [ E ]  Enter / Space / Z", true);
    drawChoiceBtn(xText + btnW + gapBtn, "NO — leave it", "[ N ]  [ S ]  (try again later)", false);
    y += btnH + 14.f;

    drawText(window, font, szFoot, "Y = try stash it (might shove solar/lunar to LTS). N = nah, ask me again later.",
             xText, y, lootMulAlpha(sf::Color(150, 155, 175), fadeM));

    drawLootScanlines(window, card, fadeM);
}

static void renderFrame(sf::RenderWindow& window, const sf::Font& font, const SharedMemory* s) {
    const float W = (float)window.getSize().x;
    const float H = (float)window.getSize().y;
    gUiScale = std::clamp(std::min(W, H) / 720.f, 0.84f, 1.24f);
    window.clear(sf::Color(10, 12, 11));

    const float margin = 3.f * (float)kUiGrid;
    const float hudBandH = 4.f * (float)kUiGrid;
    /* Tall enough for ~6 weapon rows + header/footer without squishing the battlefield excessively. */
    const float bottomH =
        std::clamp(std::max(H * 0.26f, 210.f * gUiScale), 188.f * gUiScale, 300.f);
    const float rBattleH = H - bottomH - hudBandH - 2.f * margin;
    sf::FloatRect rBattle(margin, margin, W - 2.f * margin, rBattleH);
    const float bottomY = margin + rBattleH + hudBandH;

    drawRetroWindow(window, rBattle, sf::Color(6, 8, 7), false, true);
    {
        float inset = (float)kUiGrid;
        sf::FloatRect rBattleIn(rBattle.left + inset, rBattle.top + inset, rBattle.width - 2.f * inset,
                                rBattle.height - 2.f * inset);
        drawCtBorder(window, rBattleIn, kRetroEdgeLight, 1.f);
    }
    float battleShakeX = 0.f, battleShakeY = 0.f;
    if (s->dmgPopupKind != 0 && monoBeforeEnd(&s->dmgPopupEndMono)) {
        double a = monoAsDouble(&s->dmgPopupStartMono);
        double b = monoAsDouble(&s->dmgPopupEndMono);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double tn = monoAsDouble(&now);
        double tr = (b > a) ? (tn - a) / (b - a) : 1.0;
        if (tr < 0) tr = 0;
        if (tr > 1) tr = 1;
        float falloff = (float)(1.0 - tr);
        if (s->dmgPopupValue >= 22 || s->dmgPopupKind == 2) {
            double ph = tn * 72.0;
            battleShakeX = (float)(std::sin(ph) * 4.2 * falloff);
            battleShakeY = (float)(std::cos(ph * 1.31) * 2.8 * falloff);
        }
    }
    drawBattlefield(window, font, rBattle, s, battleShakeX, battleShakeY);

    hudUpdateHpSmoothing(s);

    {
        float hbH = std::max((float)(6 * kUiGrid) + 6.f, 32.f * gUiScale);
        sf::FloatRect hud(rBattle.left + (float)(2 * kUiGrid), rBattle.top + (float)(2 * kUiGrid),
                          rBattle.width - (float)(4 * kUiGrid), hbH);
        sf::RectangleShape hudBg({hud.width, hud.height});
        hudBg.setPosition(hud.left, hud.top);
        hudBg.setFillColor(sf::Color(0, 0, 0, 191));
        window.draw(hudBg);
        sf::RectangleShape hudGoldBot({hud.width, 1.f});
        hudGoldBot.setPosition(hud.left, hud.top + hud.height - 1.f);
        hudGoldBot.setFillColor(sf::Color(245, 192, 24, 230));
        window.draw(hudGoldBot);

        float hx = hud.left + (float)(2 * kUiGrid);
        float hy = hud.top + (float)(kUiGrid + 3);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d/%d", s->enemiesKilled, cr::enemiesToWin);
        hx += drawHudStatChip(window, font, hx, hy, "KILLS", buf, kUiGold);
        snprintf(buf, sizeof(buf), "%d", countAliveEnemies(s));
        hx += drawHudStatChip(window, font, hx, hy, "HOSTILE", buf, sf::Color(255, 88, 98));
        snprintf(buf, sizeof(buf), "%d", s->concurrentCap);
        hx += drawHudStatChip(window, font, hx, hy, "CAP", buf, sf::Color(205, 202, 194));
        snprintf(buf, sizeof(buf), "%d", s->rollNumber);
        hx += drawHudStatChip(window, font, hx, hy, "SEED", buf, sf::Color(205, 202, 194));
        snprintf(buf, sizeof(buf), "%.2f", s->staminaPace);
        drawHudStatChip(window, font, hx, hy, "PACE", buf, sf::Color(205, 202, 194));

        float hudSubY = hud.top + (float)(4 * kUiGrid + 6);
        float hudSubX = hud.left + (float)(2 * kUiGrid);
        unsigned hintSz = uiOverlaySize(kTypeHint);
        if (s->ultimateActive) {
            drawText(window, font, hintSz, "ULT — ASP frozen", hudSubX, hudSubY, sf::Color(255, 228, 96));
        } else if (s->fxSpecialWeaponId >= 0 && monoBeforeEnd(&s->fxSpecialEndMono)) {
            drawText(window, font, hintSz, s->fxSpecialText, hudSubX, hudSubY, sf::Color(190, 240, 255));
        }
    }

    sf::RectangleShape hudSep({W - 2.f * margin, hudBandH});
    hudSep.setPosition(margin, margin + rBattleH);
    hudSep.setFillColor(sf::Color(0, 0, 0, 140));
    window.draw(hudSep);
    sf::RectangleShape hudSepGold({W - 2.f * margin, 1.f});
    hudSepGold.setPosition(margin, margin + rBattleH + hudBandH - 1.f);
    hudSepGold.setFillColor(sf::Color(245, 192, 24, 160));
    window.draw(hudSepGold);

    float gap = (float)(2 * kUiGrid);
    /* CMD panel strictly wider than party/foe panel (~58% / 42%). */
    float usableW = W - 2.f * margin - gap;
    float menuW = usableW * 0.58f;
    float statusW = usableW - menuW;
    const float kStatusHudMin = 200.f;
    if (statusW < kStatusHudMin && usableW > kStatusHudMin + 280.f) {
        statusW = kStatusHudMin;
        menuW = usableW - statusW;
    }
    float statusX = margin + menuW + gap;

    sf::FloatRect rMenu(margin, bottomY, menuW, bottomH);
    sf::FloatRect rStatus(statusX, bottomY, statusW, bottomH);

    int curActor = s->currentActorId;
    int playerPhase =
        (curActor >= 0 && curActor < s->numPlayers && s->players[curActor].isAlive);

    if (s->numPlayers >= 2) {
        char mp[140];
        const char* an = "(n/a)";
        if (curActor >= 0 && curActor < s->numPlayers && s->players[curActor].isAlive)
            an = s->players[curActor].name;
        snprintf(mp, sizeof(mp),
                 "MULTIPLAYER hot-seat — pass keyboard — ACTIVE: %s (hero %d of %d)", an, curActor + 1,
                 s->numPlayers);
        drawText(window, font, uiOverlaySize(kTypeHint), mp, margin + 10.f, margin + rBattleH + 5.f,
                 sf::Color(140, 214, 255));
    }

    int tnavUi = 0, wnavUi = 0, snavUi = 0;
    pthread_mutex_lock(&gInputStateMu);
    tnavUi = gTargetNavEnabled;
    wnavUi = gWeaponNavEnabled;
    snavUi = gSwapNavEnabled;
    pthread_mutex_unlock(&gInputStateMu);
    if (tnavUi)
        gCtxHintFade = 1.f;
    else
        gCtxHintFade = std::max(0.f, gCtxHintFade - 0.048f);
    if (playerPhase && wnavUi)
        gWeaponPanelSlide = std::min(1.f, gWeaponPanelSlide + 0.16f);
    else
        gWeaponPanelSlide = std::max(0.f, gWeaponPanelSlide - 0.22f);
    if (playerPhase && snavUi)
        gSwapPanelSlide = std::min(1.f, gSwapPanelSlide + 0.16f);
    else
        gSwapPanelSlide = std::max(0.f, gSwapPanelSlide - 0.22f);

    const float kCtxHintH = std::max(22.f, 19.f * gUiScale);
    drawTargetPickHintBar(window, font, margin, W, bottomY, kCtxHintH, gCtxHintFade);
    if (snavUi) {
        int slHint = 0;
        pthread_mutex_lock(&gInputStateMu);
        slHint = gSwapLtsCount;
        pthread_mutex_unlock(&gInputStateMu);
        const char* swapMsg =
            (slHint <= 0) ? "* Long-term storage is empty"
                          : "* Select a weapon to swap in - Esc to cancel";
        drawSwapPickHintBar(window, font, margin, W, bottomY, kCtxHintH, 1.f, swapMsg);
    }

    drawUiPanelDark(window, rMenu);
    drawUiPanelDark(window, rStatus);

    int partyFlashIdx =
        (s->hitFlashPlayerIdx >= 0 && monoBeforeEnd(&s->hitFlashPlayerEndMono)) ? s->hitFlashPlayerIdx : -1;
    int healGlowIdx = (s->fxHealPlayerIdx >= 0 && monoBeforeEnd(&s->fxHealEndMono)) ? s->fxHealPlayerIdx : -1;

    {
        const float mIn = (float)(2 * kUiGrid);
        float mx = rMenu.left + mIn;
        float listCardL = rMenu.left + mIn;
        float listCardW = rMenu.width - 2.f * mIn;
        sf::Vector2i mpi = sf::Mouse::getPosition(window);
        sf::Vector2f mouse((float)mpi.x, (float)mpi.y);

        sf::FloatRect portrait(rMenu.left + rMenu.width - mIn - 30.f, rMenu.top + mIn, 28.f, 28.f);
        sf::Color portTint(28, 30, 36, 245);
        if (playerPhase) {
            static const sf::Color ph[cr::maxPlayers] = {
                {42, 130, 220, 255}, {235, 72, 102, 255}, {235, 198, 64, 255}, {88, 215, 118, 255}};
            portTint = ph[curActor % cr::maxPlayers];
            portTint.a = 238;
        }
        sf::RectangleShape portBg({portrait.width, portrait.height});
        portBg.setPosition(portrait.left, portrait.top);
        portBg.setFillColor(portTint);
        window.draw(portBg);
        drawUiGoldGlowBorder(window, portrait);
        char initial[8] = "?";
        if (playerPhase) {
            const char* nm = s->players[curActor].name;
            initial[0] = (nm && nm[0]) ? nm[0] : (char)('0' + curActor);
            initial[1] = '\0';
        }
        drawTextShadow(window, font, uiOverlaySize(12), initial, portrait.left + 8.f, portrait.top + 6.f,
                       sf::Color(255, 252, 235), sf::Color(14, 12, 10));

        int menuIdx = 0, tnav = 0, wnav = 0, snav = 0, unav = 0;
        int wOwnSnap = 0, wCursorSnap = 0;
        int wListSnap[weaponIds::count];
        int ultCurSnap = 0;
        int sLtsSnap = 0, sCurSnap = 0;
        pthread_mutex_lock(&gInputStateMu);
        menuIdx = gActionMenuIndex;
        tnav = gTargetNavEnabled;
        wnav = gWeaponNavEnabled;
        snav = gSwapNavEnabled;
        unav = gUltMenuEnabled;
        wOwnSnap = gWeaponOwnCount;
        wCursorSnap = gWeaponCursor;
        for (int wi = 0; wi < wOwnSnap; wi++) wListSnap[wi] = gWeaponList[wi];
        ultCurSnap = gUltCursor;
        sLtsSnap = gSwapLtsCount;
        sCurSnap = gSwapCursor;
        pthread_mutex_unlock(&gInputStateMu);

        bool subWeapon = playerPhase && wnav;
        bool subUlt = playerPhase && unav;
        bool subSwap = playerPhase && snav;
        bool showMainGrid = playerPhase && !subWeapon && !subUlt && !subSwap;

        if (subWeapon && !gPrevWeaponSubmenu)
            gWeaponScrollPx = 0.f;
        if (subSwap && !gPrevSwapSubmenu)
            gSwapScrollPx = 0.f;

        unsigned secSz = uiOverlaySize(kTypeLabel) + 2;
        const char* secTitle = "ACTIONS";
        if (subUlt)
            secTitle = "ULTIMATE";
        else if (subWeapon)
            secTitle = "WEAPON";
        else if (subSwap)
            secTitle = "SWAP";

        drawText(window, font, secSz, secTitle, mx, rMenu.top + mIn, sf::Color(198, 192, 176));
        float ruleY = rMenu.top + mIn + (float)secSz + 6.f;
        drawGoldSectionRule(window, mx, ruleY, listCardW);
        float listY = ruleY + 11.f;

        float footerReserve = 46.f;
        float availH = std::max(48.f, rMenu.height - (listY - rMenu.top) - footerReserve);
        int nRows = 4;
        float lineStep = std::max(22.f, availH / (float)nRows);

        unsigned metaSz = uiOverlaySize(kTypeMeta);
        float pickRowGap = std::max(15.f, uiOverlaySize(kTypeBody) * 0.98f);

        float listH;
        if (subWeapon) {
            const float kWeaponFooter = 44.f;
            float rowGuess = uiOverlaySize(kTypeBody) + uiOverlaySize(kTypeMeta) + 22.f;
            int nr = (wOwnSnap <= 0) ? 1 : (wOwnSnap + 1);
            listH = (float)nr * rowGuess + kWeaponFooter + 14.f;
        } else if (subSwap) {
            const float kSwapFooter = 44.f;
            float rowGuess = uiOverlaySize(kTypeBody) + uiOverlaySize(kTypeMeta) + 22.f;
            int nr = (sLtsSnap <= 0) ? 1 : (sLtsSnap + 1);
            listH = (float)nr * rowGuess + kSwapFooter + 14.f;
        } else if (subUlt)
            listH = pickRowGap * 6.8f;
        else
            listH = (float)nRows * lineStep + 10.f;
        listH = std::min(listH, std::max(56.f, availH - 4.f));

        float innerPad = (float)kUiGrid;

        if (subUlt) {
            float uy = listY + 4.f;
            const Entity* plu = &s->players[curActor];
            int ready = canUseUltimate(plu);
            drawText(window, font, uiOverlaySize(kTypeBody),
                     ready ? "Solar Core + Lunar Blade: ready." : "Requires Solar Core + Lunar Blade in inventory.",
                     mx + 2.f, uy, ready ? sf::Color(140, 235, 175) : sf::Color(235, 165, 145));
            uy += pickRowGap * 1.35f;
            const char* castLabel = "Cast (ASP pause)";
            float rowHt = pickRowGap - 2.f;
            for (int row = 0; row < gUltMenuRows; row++) {
                bool sel = ultCurSnap == row;
                float ry = uy + (float)row * pickRowGap;
                sf::FloatRect rowR(mx + 1.f, ry, listCardW - 2.f, rowHt);
                bool hover = rowR.contains(mouse);
                drawCmdRowChrome(window, rowR, sel, hover && !sel);
                const char* txt = (row == 0) ? castLabel : "<< Back to actions";
                sf::Color rowCol = sel ? sf::Color(255, 250, 235) : sf::Color(185, 180, 172);
                drawText(window, font, uiOverlaySize(kTypeBody), txt, mx + 16.f, ry + 4.f, rowCol);
            }
        } else if (subWeapon) {
            float slideDy = (1.f - gWeaponPanelSlide) * 28.f * gUiScale;
            const Entity* plw = &s->players[curActor];
            int equippedWid = primaryEquippedWeaponId(&plw->inv);
            unsigned rowSz = uiOverlaySize(kTypeBody);
            unsigned metaSmall = uiOverlaySize(kTypeMeta);
            const float kFootReserve = 40.f;
            float footerTop = rMenu.top + rMenu.height - mIn - kFootReserve;
            float innerTop = listY + 8.f - slideDy;
            float headerFloor = rMenu.top + mIn + (float)secSz + 26.f;
            innerTop = std::max(innerTop, headerFloor);

            auto drawWeaponFooter = [&]() {
                drawGoldSectionRule(window, mx, footerTop - 8.f, listCardW);
                const char* ft = "[ Enter / Z ] Equip     [ Esc / X ] Cancel";
                unsigned fSz = uiOverlaySize(kTypeMeta);
                float fw = measureTextWidth(font, fSz, ft);
                drawText(window, font, fSz, ft, mx + (listCardW - fw) * 0.5f, footerTop - 1.f,
                         sf::Color(165, 148, 98));
            };

            if (wOwnSnap <= 0) {
                const char* emptyMsg = "* No weapons in inventory";
                unsigned eSz = uiOverlaySize(kTypeBody);
                float ew = measureTextWidth(font, eSz, emptyMsg);
                float cy = innerTop + std::max(12.f, (footerTop - innerTop - (float)eSz) * 0.4f);
                drawTextItalic(window, font, eSz, emptyMsg, mx + (listCardW - ew) * 0.5f, cy,
                               sf::Color(175, 158, 118));
                drawWeaponFooter();
            } else {
                /* Fixed row height + scroll — never shrink rows to fit N items (that broke after row 1). */
                const float kRowPad = 12.f;
                float rowStep =
                    std::max((float)rowSz + (float)metaSmall + 8.f + kRowPad, 36.f * gUiScale);

                float viewportTop = innerTop;
                float viewportBot = footerTop - 12.f;
                float viewportH = std::max(80.f, viewportBot - viewportTop);

                int nSlots = wOwnSnap + 1;
                float contentH = (float)nSlots * rowStep;
                float scrollMax = std::max(0.f, contentH - viewportH);

                float selTop = (float)wCursorSnap * rowStep;
                float selBot = selTop + rowStep;
                if (selTop < gWeaponScrollPx)
                    gWeaponScrollPx = selTop;
                if (selBot > gWeaponScrollPx + viewportH)
                    gWeaponScrollPx = selBot - viewportH;
                gWeaponScrollPx = std::clamp(gWeaponScrollPx, 0.f, std::max(0.f, scrollMax));

                unsigned indSz = uiOverlaySize(kTypeMeta);
                if (scrollMax > 2.f && gWeaponScrollPx > 4.f) {
                    float cx = mx + listCardW * 0.5f;
                    float span = std::max(5.f, (float)indSz * 0.42f);
                    drawScrollArrowUp(window, cx, viewportTop + span * 0.2f, span, sf::Color(188, 162, 88, 210));
                }
                if (scrollMax > 2.f && gWeaponScrollPx < scrollMax - 4.f) {
                    float cx = mx + listCardW * 0.5f;
                    float span = std::max(5.f, (float)indSz * 0.42f);
                    drawScrollArrowDown(window, cx, viewportBot - 2.f, span, sf::Color(188, 162, 88, 210));
                }

                for (int idx = 0; idx < nSlots; idx++) {
                    float ry = viewportTop + (float)idx * rowStep - gWeaponScrollPx;
                    if (ry + rowStep < viewportTop || ry > viewportBot)
                        continue;

                    bool isBack = (idx == wOwnSnap);
                    bool sel = (wCursorSnap == idx);
                    sf::FloatRect rowR(mx + 2.f, ry, listCardW - 4.f, rowStep - 2.f);
                    bool hover = rowR.contains(mouse);
                    drawCmdRowChrome(window, rowR, sel, hover && !sel);

                    if (isBack) {
                        sf::Color tc = sel ? sf::Color(255, 250, 235) : sf::Color(175, 205, 235);
                        drawText(window, font, rowSz, "<< Cancel / Back", mx + 18.f,
                                 ry + (rowStep - (float)rowSz) * 0.38f, tc);
                        continue;
                    }

                    int wid = wListSnap[idx];
                    if (wid < 0 || wid >= weaponIds::count)
                        continue;
                    const WeaponDef* wd = &weaponTable[wid];
                    int slotIdx = invSlotForWeapon(&plw->inv, wid);
                    const bool onCooldown =
                        (plw->swapInCooldownWeapon != weaponIds::none && plw->swapInCooldownWeapon == wid);

                    sf::Color nameCol = sel ? sf::Color(255, 252, 245) : sf::Color(222, 218, 210);
                    sf::Color slotCol = sel ? sf::Color(205, 198, 185) : sf::Color(158, 154, 146);
                    sf::Color statCol = sel ? sf::Color(210, 202, 178) : sf::Color(132, 128, 120);
                    if (onCooldown) {
                        nameCol = sel ? sf::Color(200, 198, 190) : sf::Color(140, 136, 128);
                        statCol = sf::Color(110, 105, 98);
                    }

                    char slotPart[24];
                    snprintf(slotPart, sizeof(slotPart), "[ %d ]", slotIdx >= 0 ? slotIdx : idx);
                    float tx = mx + 16.f;
                    drawText(window, font, metaSz, slotPart, tx, ry + 2.f, slotCol);
                    tx += measureTextWidth(font, metaSz, slotPart) + 10.f;
                    drawText(window, font, rowSz, wd->name, tx, ry + 1.f, nameCol);
                    tx += measureTextWidth(font, rowSz, wd->name) + 10.f;
                    if (wid == equippedWid)
                        drawEquippedTag(window, font, tx, ry + 2.f, 1.f);

                    char stats[112];
                    const char* typ = wd->isArtifact ? "Artifact" : "Standard";
                    if (onCooldown)
                        snprintf(stats, sizeof(stats), "DMG: %d   SPD: %d   TYPE: %s   (cooldown)", wd->damage,
                                 plw->speed, typ);
                    else
                        snprintf(stats, sizeof(stats), "DMG: %d   SPD: %d   TYPE: %s", wd->damage, plw->speed, typ);
                    drawText(window, font, metaSmall, stats, mx + 18.f, ry + (float)rowSz + 5.f, statCol);
                }
                drawWeaponFooter();
            }
        } else if (subSwap) {
            float slideDy = (1.f - gSwapPanelSlide) * 28.f * gUiScale;
            const Entity* pls = &s->players[curActor];
            unsigned rowSz = uiOverlaySize(kTypeBody);
            unsigned metaSmall = uiOverlaySize(kTypeMeta);
            const float kFootReserve = 40.f;
            float footerTop = rMenu.top + rMenu.height - mIn - kFootReserve;
            float innerTop = listY + 8.f - slideDy;
            float headerFloor = rMenu.top + mIn + (float)secSz + 26.f;
            innerTop = std::max(innerTop, headerFloor);

            auto drawSwapFooter = [&]() {
                drawGoldSectionRule(window, mx, footerTop - 8.f, listCardW);
                const char* ft = "[ Enter / Z ] Swap in     [ Esc / X ] Cancel";
                unsigned fSz = uiOverlaySize(kTypeMeta);
                float fw = measureTextWidth(font, fSz, ft);
                drawText(window, font, fSz, ft, mx + (listCardW - fw) * 0.5f, footerTop - 1.f,
                         sf::Color(165, 148, 98));
            };

            const char* hn = (pls->name[0] != '\0') ? pls->name : "?";
            float nameY = innerTop;
            float chevCY = nameY + (float)rowSz * 0.45f;
            drawGoldRightChevronTip(window, mx + 20.f, chevCY, (float)rowSz * 0.38f, kUiGold);
            float nameX = mx + 30.f;
            drawText(window, font, rowSz, hn, nameX, nameY, kUiGold);
            unsigned tagSz = uiOverlaySize(kTypeMeta);
            float nameW = measureTextWidth(font, rowSz, hn);
            drawText(window, font, tagSz, "[ACTIVE]", nameX + nameW + 10.f, innerTop + 1.f, kUiGold);

            float bannerBelow = innerTop + (float)rowSz + 10.f;

            if (sLtsSnap <= 0) {
                const char* emptyMsg = "* Long-term storage is empty";
                unsigned eSz = uiOverlaySize(kTypeBody);
                float ew = measureTextWidth(font, eSz, emptyMsg);
                float cy = bannerBelow + std::max(12.f, (footerTop - bannerBelow - (float)eSz) * 0.35f);
                drawTextItalic(window, font, eSz, emptyMsg, mx + (listCardW - ew) * 0.5f, cy,
                               sf::Color(175, 158, 118));
                drawSwapFooter();
            } else {
                const float kRowPad = 12.f;
                float rowStep =
                    std::max((float)rowSz + (float)metaSmall + 8.f + kRowPad, 36.f * gUiScale);

                float viewportTop = bannerBelow;
                float viewportBot = footerTop - 12.f;
                float viewportH = std::max(80.f, viewportBot - viewportTop);

                int nSlots = sLtsSnap + 1;
                float contentH = (float)nSlots * rowStep;
                float scrollMax = std::max(0.f, contentH - viewportH);

                float selTop = (float)sCurSnap * rowStep;
                float selBot = selTop + rowStep;
                if (selTop < gSwapScrollPx)
                    gSwapScrollPx = selTop;
                if (selBot > gSwapScrollPx + viewportH)
                    gSwapScrollPx = selBot - viewportH;
                gSwapScrollPx = std::clamp(gSwapScrollPx, 0.f, std::max(0.f, scrollMax));

                unsigned indSz = uiOverlaySize(kTypeMeta);
                if (scrollMax > 2.f && gSwapScrollPx > 4.f) {
                    float cx = mx + listCardW * 0.5f;
                    float span = std::max(5.f, (float)indSz * 0.42f);
                    drawScrollArrowUp(window, cx, viewportTop + span * 0.2f, span, sf::Color(188, 162, 88, 210));
                }
                if (scrollMax > 2.f && gSwapScrollPx < scrollMax - 4.f) {
                    float cx = mx + listCardW * 0.5f;
                    float span = std::max(5.f, (float)indSz * 0.42f);
                    drawScrollArrowDown(window, cx, viewportBot - 2.f, span, sf::Color(188, 162, 88, 210));
                }

                for (int idx = 0; idx < nSlots; idx++) {
                    float ry = viewportTop + (float)idx * rowStep - gSwapScrollPx;
                    if (ry + rowStep < viewportTop || ry > viewportBot)
                        continue;

                    bool isBack = (idx == sLtsSnap);
                    bool sel = (sCurSnap == idx);
                    sf::FloatRect rowR(mx + 2.f, ry, listCardW - 4.f, rowStep - 2.f);
                    bool hover = rowR.contains(mouse);
                    drawCmdRowChrome(window, rowR, sel, hover && !sel);

                    if (isBack) {
                        sf::Color tc = sel ? sf::Color(255, 250, 235) : sf::Color(175, 205, 235);
                        drawText(window, font, rowSz, "<< Cancel / Back", mx + 18.f,
                                 ry + (rowStep - (float)rowSz) * 0.38f, tc);
                        continue;
                    }

                    int wid = pls->lts.items[idx];
                    if (wid < 0 || wid >= weaponIds::count)
                        continue;
                    const WeaponDef* wd = &weaponTable[wid];

                    sf::Color nameCol = sel ? sf::Color(255, 252, 245) : sf::Color(222, 218, 210);
                    sf::Color slotCol = sel ? sf::Color(205, 198, 185) : sf::Color(158, 154, 146);
                    sf::Color statCol = sel ? sf::Color(210, 202, 178) : sf::Color(132, 128, 120);

                    char slotPart[24];
                    snprintf(slotPart, sizeof(slotPart), "[ %d ]", idx);
                    float tx = mx + 16.f;
                    drawText(window, font, metaSz, slotPart, tx, ry + 2.f, slotCol);
                    tx += measureTextWidth(font, metaSz, slotPart) + 10.f;
                    drawText(window, font, rowSz, wd->name, tx, ry + 1.f, nameCol);

                    char stats[140];
                    const char* typ = wd->isArtifact ? "Artifact" : "Standard";
                    snprintf(stats, sizeof(stats), "Slots: %d  DMG: %d  TYPE: %s", wd->slotSize, wd->damage, typ);
                    drawText(window, font, metaSmall, stats, mx + 18.f, ry + (float)rowSz + 5.f, statCol);
                }
                drawSwapFooter();
            }
        } else {
            const char* labels[] = {"Attack",   "Exhaust", "Heal",    "Skip",     "Ultimate",
                                    "Weapon",   "Swap",    "Quit",    nullptr};
            const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "Q"};
            const float colGap = (float)(2 * kUiGrid + 2);
            const float labelPad = 18.f;
            float twinInner = listCardW - 2.f * innerPad;
            float cellW = (twinInner - colGap) * 0.5f;
            float cell0L = listCardL + innerPad;
            float cell1L = cell0L + cellW + colGap;
            float rowH = lineStep;
            unsigned actionBodySz = (unsigned)std::lround((float)uiOverlaySize(kTypeBody) * 1.25f);
            unsigned actionKeySz = (unsigned)std::lround((float)uiOverlaySize(kTypeMeta) * 1.25f);
            float panelKeyRight = listCardL + listCardW - innerPad;

            for (int i = 0; labels[i]; i++) {
                int row = i / 2;
                int col = i % 2;
                float cellLeft = (col == 0) ? cell0L : cell1L;
                float ry = listY + row * lineStep;
                bool sel = showMainGrid && i == menuIdx;
                sf::FloatRect rowR(cellLeft, ry, cellW, rowH);
                bool hover = showMainGrid && playerPhase && rowR.contains(mouse);
                drawCmdRowChrome(window, rowR, sel, hover && !sel);
                sf::Color rowCol =
                    playerPhase ? (sel ? sf::Color(255, 250, 235) : sf::Color(185, 182, 174))
                                : sf::Color(125, 122, 118, 170);
                sf::Color keyCol =
                    playerPhase ? (sel ? sf::Color(245, 205, 96) : sf::Color(125, 120, 112))
                                : sf::Color(95, 92, 88, 150);
                float textDy = (rowH - (float)actionBodySz) * 0.42f;
                float lx = cellLeft + labelPad;
                drawText(window, font, actionBodySz, labels[i], lx, ry + textDy, rowCol);
                char kb[8];
                snprintf(kb, sizeof(kb), "%s", keys[i]);
                float kw = measureTextWidth(font, actionKeySz, kb);
                float kx = panelKeyRight - kw;
                float keyDy = (rowH - (float)actionKeySz) * 0.45f;
                drawText(window, font, actionKeySz, kb, kx, ry + keyDy, keyCol);
            }
        }

        unsigned hintSz = uiOverlaySize(kTypeHint);
        if (!playerPhase) {
            drawText(window, font, hintSz, "Enemy phase", mx, listY + listH + 6.f, sf::Color(130, 126, 118));
        }

        pthread_mutex_lock(&gPromptMu);
        int pl = gPromptLines;
        char l0[256], l1[256], l2[256], ow[256];
        strncpy(l0, gPromptLine0, sizeof(l0));
        strncpy(l1, gPromptLine1, sizeof(l1));
        strncpy(l2, gPromptLine2, sizeof(l2));
        strncpy(ow, gOwnedLine, sizeof(ow));
        l0[255] = l1[255] = l2[255] = ow[255] = '\0';
        pthread_mutex_unlock(&gPromptMu);

        float msgY = rMenu.top + rMenu.height - 46.f;
        unsigned bodySz = uiOverlaySize(kTypeBody);
        if (!tnav && !wnav && !snav) {
            if (ow[0]) drawText(window, font, hintSz, ow, mx, msgY - (float)hintSz - 4.f, sf::Color(195, 210, 245));
            if (pl >= 1) drawText(window, font, bodySz, l0, mx, msgY, sf::Color(255, 235, 160));
            if (pl >= 2) drawText(window, font, hintSz, l1, mx, msgY + (float)hintSz + 5.f, sf::Color(255, 185, 165));
            if (pl >= 3)
                drawText(window, font, hintSz, l2, mx, msgY + 2.f * ((float)hintSz + 5.f), sf::Color(255, 185, 165));
        }

        gPrevWeaponSubmenu = subWeapon ? 1 : 0;
        gPrevSwapSubmenu = subSwap ? 1 : 0;
    }

    {
        const float pad = (float)(2 * kUiGrid);
        float innerL = rStatus.left + pad;
        float innerR = rStatus.left + rStatus.width - pad;
        float innerW = innerR - innerL;
        const float nameColW = std::clamp(innerW * 0.34f, 54.f, 98.f);
        const float statX = innerL + nameColW + (float)kUiGrid;
        const float statW = std::max(44.f, innerR - statX);
        unsigned labSz = uiOverlaySize(kTypeLabel);
        float hpLW = measureTextWidth(font, labSz, "HP");
        float stLW = measureTextWidth(font, labSz, "ST");
        float labColW = std::max(hpLW, stLW) + 4.f;
        float numReserve =
            measureTextWidth(font, uiOverlaySize(kTypeMeta), "888 / 888") + 10.f;
        float barWParty = statW - labColW - numReserve;
        if (barWParty < 36.f) barWParty = 36.f;
        float barX0 = statX + labColW;
        const float kHpBarH = 7.f;
        const float kStBarH = 6.f;
        const float kRowPairGap = 3.f;
        unsigned entSz = uiOverlaySize(kTypeEntity);
        const float kNameH = (float)entSz + 3.f;
        const float kMetaAfter = 11.f;
        float sx = innerL;
        float sy = rStatus.top + (float)(2 * kUiGrid) + 4.f;
        unsigned secPartySz = uiOverlaySize(kTypeLabel) + 2;

        drawText(window, font, secPartySz, "PARTY", sx, sy, sf::Color(212, 206, 188));
        sy += (float)secPartySz + 5.f;
        drawGoldSectionRule(window, sx, sy, innerW);
        sy += 9.f;

        for (int i = 0; i < s->numPlayers; i++) {
            const Entity* p = &s->players[i];
            int active = (i == curActor && curActor < s->numPlayers);
            sf::Color nameC = active ? sf::Color(255, 232, 120) : sf::Color(228, 236, 220);
            if (partyFlashIdx == i) nameC = sf::Color(255, 150, 150);
            if (healGlowIdx == i) nameC = sf::Color(160, 255, 200);
            char buf[192];
            char ca[20], cb[20], cc[20], cd[20];
            if (!p->isAlive) {
                snprintf(buf, sizeof(buf), "%s (down)", p->name);
                drawText(window, font, uiOverlaySize(kTypeBody), buf, sx, sy, sf::Color(115, 118, 128));
                sy += 17.f;
                continue;
            }
            snprintf(buf, sizeof(buf), "%s", p->name);
            drawText(window, font, entSz, buf, sx, sy, nameC);
            float rowY = sy + kNameH + 2.f;
            fmtNumCompact(ca, sizeof(ca), p->hp);
            fmtNumCompact(cb, sizeof(cb), p->maxHp);
            fmtNumCompact(cc, sizeof(cc), (int)std::lround((double)p->stamina));
            fmtNumCompact(cd, sizeof(cd), p->maxStamina);
            drawText(window, font, labSz, "HP", statX, rowY + 1.f, sf::Color(140, 136, 128));
            drawHudHpTrackAndFill(window, barX0, rowY + 1.f, barWParty, kHpBarH, gHudHpSmoothParty[i]);
            drawSlashHpNumbers(window, font, innerR - 4.f, rowY + 1.f, ca, cb, sf::Color(195, 190, 182));
            rowY += kHpBarH + kRowPairGap;
            drawText(window, font, labSz, "ST", statX, rowY + 1.f, sf::Color(140, 136, 128));
            float stF = p->maxStamina > 0 ? p->stamina / (float)p->maxStamina : 0.f;
            drawHudStaminaBar(window, barX0, rowY + 1.f, barWParty, kStBarH, stF);
            drawSlashHpNumbers(window, font, innerR - 4.f, rowY + 1.f, cc, cd, sf::Color(175, 168, 148));
            rowY += kStBarH + kRowPairGap + 3.f;
            char invLine[64];
            snprintf(invLine, sizeof(invLine), "DMG %d  SPD %d", p->damage, p->speed);
            drawText(window, font, uiOverlaySize(kTypeMeta), invLine, sx, rowY, sf::Color(110, 108, 102));
            sy = rowY + kMetaAfter;
        }

        sy += 16.f;

        float statusBottom = rStatus.top + rStatus.height - 6.f;
        float logTop = sy + 10.f;
        float logPanelH = statusBottom - logTop;
        const float logMinH = 38.f;
        if (logPanelH < logMinH) {
            logPanelH = logMinH;
            logTop = statusBottom - logPanelH;
        }
        if (logTop < sy + 4.f) logTop = sy + 4.f;
        logPanelH = statusBottom - logTop;

        drawGoldSectionRule(window, sx, logTop + 2.f, innerW);
        drawText(window, font, uiOverlaySize(kTypeLabel), "LOG", sx, logTop + 12.f, sf::Color(188, 182, 168));
        int lh = s->logHead;
        float lineStepLog = (float)(2 * kUiGrid) + 3.f;
        int logRows = (int)((logPanelH - 28.f) / lineStepLog);
        logRows = std::max(2, std::min(logRows, cr::logLines));
        float ly = logTop + 26.f;
        for (int row = cr::logLines - logRows; row < cr::logLines; row++) {
            int idx = (lh + row) % cr::logLines;
            drawText(window, font, uiOverlaySize(kTypeMeta), s->logLines[idx], sx, ly, sf::Color(175, 188, 168));
            ly += lineStepLog;
        }
    }

    drawWeaponDropPopupOnTop(window, font, W, H, s);
    drawEclipseRelicPickupPopupOnTop(window, font, W, H);
    /* Draw kill ribbon above loot scrim. */
    drawEnemyKillBannerOnTop(window, font, W, s);
    /* Ultimate denied: centered red banner + slight dim; drawn last so it reads clearly. */
    drawUltimateDeniedOverlayOnTop(window, font, W, H, s);
}

static int readEnemySlotKey(SharedMemory* shm, int heroIdx) {
    (void)heroIdx;
    int list[cr::maxEnemies];
    int nAlive = 0;
    pthread_mutex_lock(&shm->stateLock);
    for (int s = 0; s < cr::maxEnemies; s++) {
        if (shm->enemies[s].isAlive) list[nAlive++] = s;
    }
    pthread_mutex_unlock(&shm->stateLock);
    if (nAlive == 0) {
        setTargetNavEnabled(0);
        setUiPrompt("No targets.", "", "");
        return -1;
    }
    pthread_mutex_lock(&gInputStateMu);
    gTargetCount = nAlive;
    for (int i = 0; i < nAlive; i++) gTargetList[i] = list[i];
    gTargetCursor = 0;
    gTargetNavEnabled = 1;
    pthread_mutex_unlock(&gInputStateMu);
    pthread_mutex_lock(&shm->stateLock);
    shm->uiTargetEnemySlot = list[0];
    clock_gettime(CLOCK_MONOTONIC, &shm->uiTargetEndMono);
    addMonoMs(&shm->uiTargetEndMono, 1600);
    pthread_mutex_unlock(&shm->stateLock);

    for (;;) {
        setUiPrompt("", "", "");
        int c = dequeueKeyBlocking();
        if (c < 0) {
            setTargetNavEnabled(0);
            return -1;
        }
        if (c >= '0' && c <= '8') {
            int slot = c - '0';
            pthread_mutex_lock(&shm->stateLock);
            int alive = shm->enemies[slot].isAlive;
            if (alive) {
                shm->uiTargetEnemySlot = slot;
                clock_gettime(CLOCK_MONOTONIC, &shm->uiTargetEndMono);
                addMonoMs(&shm->uiTargetEndMono, 1200);
            }
            pthread_mutex_unlock(&shm->stateLock);
            if (!alive) {
                setUiPrompt("", "", "");
                usleep(280000);
                continue;
            }
            setTargetNavEnabled(0);
            return slot;
        }
    }
}

static int readWeaponDigit(SharedMemory* shm, int heroIdx) {
    Entity* pl = &shm->players[heroIdx];
    int have[weaponIds::count];
    memset(have, 0, sizeof(have));
    for (int j = 0; j < cr::invSize; j++) {
        int id = pl->inv.slots[j];
        if (id >= 0 && id < weaponIds::count) have[id] = 1;
    }
    int tmp[weaponIds::count];
    int n = 0;
    for (int id = 0; id < weaponIds::count; id++)
        if (have[id]) tmp[n++] = id;
    pthread_mutex_lock(&gInputStateMu);
    gWeaponOwnCount = n;
    for (int i = 0; i < n; i++) gWeaponList[i] = tmp[i];
    gWeaponCount = n + 1;
    gWeaponCursor = 0;
    gWeaponNavEnabled = 1;
    pthread_mutex_unlock(&gInputStateMu);

    for (;;) {
        setUiPrompt("", "", "");
        int c = dequeueKeyBlocking();
        if (c < 0) {
            setWeaponNavEnabled(0);
            return -1;
        }
        if (c == 'b' || c == 'B') {
            setWeaponNavEnabled(0);
            return -2;
        }
        if (c >= '0' && c <= '8') {
            int id = c - '0';
            if (!have[id]) {
                setUiPrompt("", "", "");
                usleep(280000);
                continue;
            }
            setWeaponNavEnabled(0);
            return id;
        }
    }
}

static int readSwapLtsIndex(SharedMemory* shm, int heroIdx) {
    Entity* pl = &shm->players[heroIdx];
    pthread_mutex_lock(&shm->stateLock);
    int n = pl->lts.count;
    pthread_mutex_unlock(&shm->stateLock);

    if (n <= 0) {
        pthread_mutex_lock(&gInputStateMu);
        gSwapNavEnabled = 1;
        gSwapLtsCount = 0;
        gSwapCount = 0;
        gSwapCursor = 0;
        pthread_mutex_unlock(&gInputStateMu);
        (void)dequeueKeyBlocking();
        setSwapNavEnabled(0);
        return -2;
    }

    pthread_mutex_lock(&gInputStateMu);
    gSwapNavEnabled = 1;
    gSwapLtsCount = n;
    gSwapCount = n + 1;
    gSwapCursor = 0;
    pthread_mutex_unlock(&gInputStateMu);

    for (;;) {
        setUiPrompt("", "", "");
        int c = dequeueKeyBlocking();
        if (c < 0) {
            setSwapNavEnabled(0);
            return -1;
        }
        if (c == 'b' || c == 'B') {
            setSwapNavEnabled(0);
            return -2;
        }
        if (c >= kSwapCommitBase && c < kSwapCommitBase + cr::ltsSize) {
            int li = c - kSwapCommitBase;
            pthread_mutex_lock(&shm->stateLock);
            int nc = shm->players[heroIdx].lts.count;
            pthread_mutex_unlock(&shm->stateLock);
            if (li < 0 || li >= nc) {
                usleep(120000);
                continue;
            }
            setSwapNavEnabled(0);
            return li;
        }
    }
}

static int readUltimateMenu(SharedMemory* shm, int heroIdx) {
    (void)shm;
    setUltMenuEnabled(1);
    char a[160];
    snprintf(a, sizeof(a), "H%d — ultimate", heroIdx);
    for (;;) {
        setUiPrompt(a, "", "");
        int c = dequeueKeyBlocking();
        if (c < 0) {
            setUltMenuEnabled(0);
            return -1;
        }
        if (c == 'b' || c == 'B') {
            setUltMenuEnabled(0);
            return -2;
        }
        if (c == '!') {
            setUltMenuEnabled(0);
            return 1;
        }
        setUiPrompt(a, "Choose Cast or Back, then Enter.", "");
        usleep(220000);
    }
}

static int playerInStunWindow(SharedMemory* shm, int playerIdx) {
    pthread_mutex_lock(&shm->stateLock);
    Entity* self = &shm->players[playerIdx];
    if (!self->isStunned) {
        pthread_mutex_unlock(&shm->stateLock);
        return 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int inWin = (now.tv_sec < self->stunEnd.tv_sec ||
                 (now.tv_sec == self->stunEnd.tv_sec && now.tv_nsec < self->stunEnd.tv_nsec));
    pthread_mutex_unlock(&shm->stateLock);
    return inWin;
}

static void* playerThread(void* pv) {
    int idx = *static_cast<int*>(pv);
    SharedMemory* shm = gShm;

#ifdef __linux__
    {
        sigset_t u;
        sigemptyset(&u);
        sigaddset(&u, SIGUSR1);
        sigaddset(&u, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &u, nullptr);
    }
    pthread_mutex_lock(&shm->stateLock);
    shm->playerOsTid[idx] = (pid_t)syscall(SYS_gettid);
    pthread_mutex_unlock(&shm->stateLock);
#endif

    while (1) {
        while (sem_wait(&shm->toPlayer[idx]) == -1 && errno == EINTR) {
        }
        if (shm->humanThreadsShouldExit) break;

        pthread_mutex_lock(&shm->stateLock);
        int cur = shm->currentActorId;
        pthread_mutex_unlock(&shm->stateLock);
        if (cur != idx) continue;

        if (playerInStunWindow(shm, idx)) {
            sigset_t empty;
            sigemptyset(&empty);
            sigsuspend(&empty);
            pthread_mutex_lock(&shm->stateLock);
            cur = shm->currentActorId;
            pthread_mutex_unlock(&shm->stateLock);
            if (cur != idx) continue;
        }

        drainKeyQueue();

        if (shm->dropPending && !shm->dropDecided) {
            setMenuNavEnabled(0);
            int dw = shm->dropWeapon;
            const char* wn = (dw >= 0 && dw < weaponIds::count) ? weaponTable[dw].name : "?";
            char mid[192];
            snprintf(mid, sizeof(mid), "On the ground: %s", wn);
            setUiPrompt("WEAPON DROPPED - choose now", mid, "Y/E take | N/S skip | Enter Space Z | legacy y/n");
            int yn = dequeueKeyBlocking();
            if (yn < 0) {
                pthread_mutex_lock(&shm->stateLock);
                shm->dropPlayerTakes = 0;
                shm->dropDecided = 1;
                pthread_mutex_unlock(&shm->stateLock);
            } else {
                pthread_mutex_lock(&shm->stateLock);
                shm->dropPlayerTakes = (yn == 'y' || yn == 'Y');
                shm->dropDecided = 1;
                pthread_mutex_unlock(&shm->stateLock);
            }
        }

        pthread_mutex_lock(&shm->stateLock);
        int relicOnGround = shm->eclipseRelicOnGround;
        pthread_mutex_unlock(&shm->stateLock);
        if (relicOnGround) {
            setMenuNavEnabled(0);
            setUiPrompt("", "", "");
            setEclipsePickupModalVisible(1);
            int yn = dequeueKeyBlocking();
            setEclipsePickupModalVisible(0);
            if (yn == 'y' || yn == 'Y') {
                Action pact = {};
                pact.actorId = idx;
                pact.ready = 1;
                pact.type = ActionType::pickupEclipseRelic;
                pthread_mutex_lock(&shm->actionLock);
                shm->pendingAction = pact;
                pthread_mutex_unlock(&shm->actionLock);
                sem_post(&shm->humanReady);
                continue;
            }
            if (yn == 'q' || yn == 'Q') {
                if (shm->arbiterPid > 0) kill(shm->arbiterPid, SIGTERM);
                Action pact = {};
                pact.actorId = idx;
                pact.ready = 1;
                pact.type = ActionType::skip;
                pthread_mutex_lock(&shm->actionLock);
                shm->pendingAction = pact;
                pthread_mutex_unlock(&shm->actionLock);
                sem_post(&shm->humanReady);
                continue;
            }
        }

        setHeroTurnBanner(idx, &shm->players[idx]);

        Action act = {};
        for (;;) {
            act = Action{};
            act.actorId = idx;
            act.ready = 1;

            setMenuNavEnabled(1);
            int c = dequeueKeyBlocking();
            setMenuNavEnabled(0);

            if (c < 0) {
                act.type = ActionType::skip;
                break;
            }
            if (c == 'q' || c == 'Q') {
                if (shm->arbiterPid > 0) kill(shm->arbiterPid, SIGTERM);
                act.type = ActionType::skip;
                break;
            }
            if (c == '4' || c == 's' || c == 'S') {
                act.type = ActionType::skip;
                break;
            }
            if (c == '3' || c == 'h' || c == 'H') {
                act.type = ActionType::heal;
                break;
            }
            if (c == '5' || c == 'u' || c == 'U') {
                int ur = readUltimateMenu(shm, idx);
                if (ur == -2)
                    continue;
                if (ur <= 0)
                    act.type = ActionType::skip;
                else
                    act.type = ActionType::ultimate;
                break;
            }
            if (c == '1' || c == 'a' || c == 'A') {
                act.type = ActionType::strike;
                int slot = readEnemySlotKey(shm, idx);
                if (slot < 0)
                    act.type = ActionType::skip;
                else
                    act.targetId = shm->numPlayers + slot;
                break;
            }
            if (c == '2' || c == 'x' || c == 'X') {
                act.type = ActionType::exhaust;
                int slot = readEnemySlotKey(shm, idx);
                if (slot < 0)
                    act.type = ActionType::skip;
                else
                    act.targetId = shm->numPlayers + slot;
                break;
            }
            if (c == '6' || c == 'w' || c == 'W') {
                act.type = ActionType::useWeapon;
                int wid = readWeaponDigit(shm, idx);
                if (wid == -2)
                    continue;
                if (wid < 0) {
                    act.type = ActionType::skip;
                } else {
                    act.weapon = wid;
                    int slot = readEnemySlotKey(shm, idx);
                    if (slot < 0)
                        act.type = ActionType::skip;
                    else
                        act.targetId = shm->numPlayers + slot;
                }
                break;
            }
            if (c == '7' || c == 'i' || c == 'I') {
                act.type = ActionType::swapIn;
                int ltsIdx = readSwapLtsIndex(shm, idx);
                if (ltsIdx == -2)
                    continue;
                if (ltsIdx < 0)
                    act.type = ActionType::skip;
                else
                    act.ltsIndex = ltsIdx;
                break;
            }
            act.type = ActionType::skip;
            setUiPrompt("Unknown key -> skip. Use menu+Enter, 1-7, or legacy a/x/h...", "", "");
            usleep(400000);
            break;
        }

        setUiPrompt("", "", "");

        pthread_mutex_lock(&shm->actionLock);
        shm->pendingAction = act;
        pthread_mutex_unlock(&shm->actionLock);

        sem_post(&shm->humanReady);
    }
    return nullptr;
}

struct HipPresentationArg {
    sf::Font* font;
};

/* owns the window — poll events here, draw here; gameplay threads dont touch GL */
static void* hipPresentationThread(void* arg) {
    HipPresentationArg* pa = static_cast<HipPresentationArg*>(arg);
    sf::Font& font = *pa->font;

    char title[120];
    pthread_mutex_lock(&gShm->stateLock);
    int np = gShm->numPlayers;
    pthread_mutex_unlock(&gShm->stateLock);
    if (np >= 2)
        snprintf(title, sizeof(title), "Chrono Rift — HIP — Local multiplayer (%d heroes)", np);
    else
        snprintf(title, sizeof(title), "Chrono Rift — HIP");

    sf::RenderWindow window(sf::VideoMode(1280, 800), title);
    /* CHRONO_VSYNC=0 avoids SFML's "vertical sync not supported" on llvmpipe/Docker. Default: on. */
    const char* vsyncEnv = getenv("CHRONO_VSYNC");
    bool wantVsync = !(vsyncEnv && vsyncEnv[0] == '0' && vsyncEnv[1] == '\0');
    window.setVerticalSyncEnabled(wantVsync);
    window.setKeyRepeatEnabled(false);

    installHipStunHandlers();

    pthread_t players[cr::maxPlayers];
    int idx[cr::maxPlayers];
    for (int i = 0; i < gShm->numPlayers; i++) {
        idx[i] = i;
        pthread_create(&players[i], nullptr, playerThread, &idx[i]);
    }

    {
        sigset_t blk;
        sigemptyset(&blk);
        sigaddset(&blk, SIGUSR1);
        sigaddset(&blk, SIGUSR2);
        pthread_sigmask(SIG_BLOCK, &blk, nullptr);
    }

    SharedMemory snapShm{};

    while (window.isOpen()) {
        pthread_mutex_lock(&gShm->stateLock);
        GameStatus st = gShm->gameStatus;
        pthread_mutex_unlock(&gShm->stateLock);
        if (st != GameStatus::running) {
            requestUiShutdown();
            window.close();
            break;
        }

        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                pid_t ap = 0;
                pthread_mutex_lock(&gShm->stateLock);
                ap = gShm->arbiterPid;
                pthread_mutex_unlock(&gShm->stateLock);
                if (ap > 0) kill(ap, SIGTERM);
                requestUiShutdown();
                window.close();
                break;
            }
            if (ev.type == sf::Event::KeyPressed) {
                bool dropPrompt = 0;
                pthread_mutex_lock(&gShm->stateLock);
                dropPrompt = gShm->dropPending && !gShm->dropDecided;
                pthread_mutex_unlock(&gShm->stateLock);
                if (dropPrompt) {
                    sf::Keyboard::Key k = ev.key.code;
                    if (k == sf::Keyboard::E || k == sf::Keyboard::Y) {
                        enqueueKey('y');
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::N) {
                        enqueueKey('n');
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        enqueueKey('y');
                        continue;
                    }
                    continue;
                }
                if (eclipsePickupModalIsVisible()) {
                    sf::Keyboard::Key k = ev.key.code;
                    if (k == sf::Keyboard::E || k == sf::Keyboard::Y) {
                        enqueueKey('y');
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::N) {
                        enqueueKey('n');
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        enqueueKey('y');
                        continue;
                    }
                    continue;
                }
                pthread_mutex_lock(&gInputStateMu);
                int tnav = gTargetNavEnabled;
                int wnav = gWeaponNavEnabled;
                int snav = gSwapNavEnabled;
                int unav = gUltMenuEnabled;
                int nav = gMenuNavEnabled;
                pthread_mutex_unlock(&gInputStateMu);
                sf::Keyboard::Key k = ev.key.code;

                if (tnav) {
                    if (k == sf::Keyboard::W || k == sf::Keyboard::Up || k == sf::Keyboard::A ||
                        k == sf::Keyboard::Left) {
                        targetNavDelta(-1, gShm);
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::Down || k == sf::Keyboard::D ||
                        k == sf::Keyboard::Right) {
                        targetNavDelta(1, gShm);
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        int slot = targetNavConfirmSlot();
                        if (slot >= 0) enqueueKey(static_cast<unsigned char>('0' + slot));
                        continue;
                    }
                    {
                        int c = mapKeyboardToAscii(k, ev.key.shift);
                        if (c >= '0' && c <= '8') {
                            enqueueKey(c);
                            continue;
                        }
                    }
                    continue;
                }
                if (unav) {
                    if (k == sf::Keyboard::W || k == sf::Keyboard::Up || k == sf::Keyboard::A ||
                        k == sf::Keyboard::Left) {
                        ultMenuDelta(-1);
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::Down || k == sf::Keyboard::D ||
                        k == sf::Keyboard::Right) {
                        ultMenuDelta(1);
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        int ua = ultMenuConfirmAction();
                        if (ua == -2)
                            enqueueKey('b');
                        else if (ua == 1)
                            enqueueKey('!');
                        continue;
                    }
                    continue;
                }
                if (snav) {
                    if (k == sf::Keyboard::Escape || k == sf::Keyboard::X) {
                        enqueueKey('b');
                        continue;
                    }
                    int sl = 0;
                    pthread_mutex_lock(&gInputStateMu);
                    sl = gSwapLtsCount;
                    pthread_mutex_unlock(&gInputStateMu);
                    if (sl > 0) {
                        if (k == sf::Keyboard::W || k == sf::Keyboard::Up || k == sf::Keyboard::A ||
                            k == sf::Keyboard::Left) {
                            swapNavDelta(-1);
                            continue;
                        }
                        if (k == sf::Keyboard::S || k == sf::Keyboard::Down || k == sf::Keyboard::D ||
                            k == sf::Keyboard::Right) {
                            swapNavDelta(1);
                            continue;
                        }
                        if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                            int li = swapNavConfirmLtsIndex();
                            if (li == -2)
                                enqueueKey('b');
                            else if (li >= 0)
                                enqueueKey(kSwapCommitBase + li);
                            continue;
                        }
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        enqueueKey(' ');
                        continue;
                    }
                    {
                        int c = mapKeyboardToAscii(k, ev.key.shift);
                        if (c > 0) enqueueKey(c);
                    }
                    continue;
                }
                if (wnav) {
                    if (k == sf::Keyboard::Escape || k == sf::Keyboard::X) {
                        enqueueKey('b');
                        continue;
                    }
                    if (k == sf::Keyboard::W || k == sf::Keyboard::Up || k == sf::Keyboard::A ||
                        k == sf::Keyboard::Left) {
                        weaponNavDelta(-1);
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::Down || k == sf::Keyboard::D ||
                        k == sf::Keyboard::Right) {
                        weaponNavDelta(1);
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        int wid = weaponNavConfirmId();
                        if (wid == -2)
                            enqueueKey('b');
                        else if (wid >= 0)
                            enqueueKey(static_cast<unsigned char>('0' + wid));
                        continue;
                    }
                    {
                        int c = mapKeyboardToAscii(k, ev.key.shift);
                        if (c >= '0' && c <= '8') {
                            enqueueKey(c);
                            continue;
                        }
                    }
                    continue;
                }
                if (nav) {
                    if (k == sf::Keyboard::W || k == sf::Keyboard::Up) {
                        menuNavGrid(-1, 0);
                        continue;
                    }
                    if (k == sf::Keyboard::S || k == sf::Keyboard::Down) {
                        menuNavGrid(1, 0);
                        continue;
                    }
                    if (k == sf::Keyboard::A || k == sf::Keyboard::Left) {
                        menuNavGrid(0, -1);
                        continue;
                    }
                    if (k == sf::Keyboard::D || k == sf::Keyboard::Right) {
                        menuNavGrid(0, 1);
                        continue;
                    }
                    if (k == sf::Keyboard::Return || k == sf::Keyboard::Space || k == sf::Keyboard::Z) {
                        enqueueKey(menuNavPickChar());
                        continue;
                    }
                    if (k >= sf::Keyboard::Num1 && k <= sf::Keyboard::Num7) {
                        int d = static_cast<int>(k) - static_cast<int>(sf::Keyboard::Num1);
                        pthread_mutex_lock(&gInputStateMu);
                        if (gMenuNavEnabled) gActionMenuIndex = d;
                        pthread_mutex_unlock(&gInputStateMu);
                        enqueueKey(static_cast<unsigned char>('1' + d));
                        continue;
                    }
                    if (k >= sf::Keyboard::Numpad1 && k <= sf::Keyboard::Numpad7) {
                        int d = static_cast<int>(k) - static_cast<int>(sf::Keyboard::Numpad1);
                        pthread_mutex_lock(&gInputStateMu);
                        if (gMenuNavEnabled) gActionMenuIndex = d;
                        pthread_mutex_unlock(&gInputStateMu);
                        enqueueKey(static_cast<unsigned char>('1' + d));
                        continue;
                    }
                }
                int c = mapKeyboardToAscii(ev.key.code, ev.key.shift);
                if (c > 0) enqueueKey(c);
            }
        }

        gFrame++;
        pthread_mutex_lock(&gShm->stateLock);
        memcpy(&snapShm, gShm, sizeof(SharedMemory));
        pthread_mutex_unlock(&gShm->stateLock);
        renderFrame(window, font, &snapShm);
        window.display();
        /* VSync paces the present; an extra 16–50 ms sleep capped FPS ~20–60 and felt sluggish. */
    }

    for (int i = 0; i < gShm->numPlayers; i++) pthread_join(players[i], nullptr);
    return nullptr;
}

int main() {
    const char* ev = getenv(cr::shmidEnvVar);
    if (!ev || !ev[0]) {
        fprintf(stderr, "hip: missing env %s (start via arbiter_exec)\n", cr::shmidEnvVar);
        return 1;
    }
    gShm = cr::shmSegmentAt(atoi(ev));
    if (!gShm) {
        perror("hip shmat");
        return 1;
    }

    sf::Font font;
    if (!loadUiFont(font)) {
        fprintf(stderr, "hip: could not load a UI font (tried DejaVu/Liberation/Ubuntu paths). Install fonts-dejavu-core.\n");
        cr::shmSegmentDetach(gShm);
        return 1;
    }

    HipPresentationArg pa;
    pa.font = &font;
    pthread_t presentation;
    pthread_create(&presentation, nullptr, hipPresentationThread, &pa);
    pthread_join(presentation, nullptr);

    cr::shmSegmentDetach(gShm);
    return 0;
}
