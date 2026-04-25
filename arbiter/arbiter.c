#include <ncurses.h>
#include <unistd.h>

int main() {
    initscr();
    cbreak();
    noecho();
    
    mvprintw(0, 0, "=== CHRONO RIFT TEST ===");
    mvprintw(2, 0, "Health:  [##########] 100%%");
    mvprintw(3, 0, "Stamina: [######----] 60%%");
    mvprintw(5, 0, "If you can read this, ncurses works!");
    mvprintw(7, 0, "Press 'q' to quit");
    
    refresh();
    
    timeout(-1);
    while(getch() != 'q') {}
    
    endwin();
    return 0;
}