$(function() {
    // Set layout target values
    const mobile_width = 1067;
    // Add case-insensitive contains filter
    $.expr[':'].filter = function(obj, index, meta) {
        return !meta[3] ? true : $(obj).text().normalize("NFD").replace(/[\u0300-\u036f]/g, "").toLowerCase().includes(meta[3].normalize("NFD").replace(/[\u0300-\u036f]/g, "").toLowerCase());
    }
    // Get session cookie
    let cookie;
    function getSession() {
        cookie = document.cookie.match(/session=([A-Za-z0-9]+)(?:;|$)/);
        return cookie !== null ? cookie[1] : null;
    }
    // Get current page
    const page = window.location.pathname;
    // Toggle mobile navigation
    $('#mobile-menu-button').click(function() {
        $('.nav-menu').scrollTop(0);
        $('.nav-menu').slideUp('fast');
        if($('#nav-user-menu').is(':visible')) {
            $('#nav-user-menu').slideUp('fast');
        }
        if($('#sidebar-left').is(':visible')) {
            $('#mobile-menu-button').html('menu');
            $('#sidebar-left').slideUp('fast');
            if(!$('#zoomdisplay.zoomed').length) {
                $('body').css('overflow', '');
            }
        } else {
            $('#mobile-menu-button').html('close');
            $('#sidebar-left').slideDown('fast');
            $('#sidebar-left').scrollTop(0);
            $('body').css('overflow', 'hidden');
        }
    });
    // Toggle user navigation
    $('.nav-menu-button').click(function() {
        const target = '#' + $(this).attr('id') + '-menu';
        if($('#mobile-menu-button').is(':visible')) {
            $('#mobile-menu-button').html('menu');
            if($('#sidebar-left').is(':visible')) {
                $('#sidebar-left').slideUp('fast');
            }
        }
        if($(target).is(':visible')) {
            $(target).slideUp('fast');
            if($(target).prop('scrollHeight') >= $(window).height() - $('#navigation').height() && !$('#zoomdisplay.zoomed').length) {
                $('body').css('overflow', '');
            }
        } else {
            $('.nav-menu').slideUp('fast');
            $(target).slideDown('fast');
            $(target).scrollTop(0);
            if($(target).prop('scrollHeight') >= $(window).height() - $('#navigation').height()) {
                $('body').css('overflow', 'hidden');
            }
        }
        return false;
    });
    // Fix normal navigation on resize
    let initial_width = $(window).outerWidth();
    $(window).resize(function() {
        if($(window).outerWidth() != initial_width) {
            initial_width = $(window).outerWidth();
            if($(window).outerWidth() > mobile_width) {
                if($('#sidebar-left').css('dipslay') != 'block') {
                    $('#sidebar-left').css('display', 'block');
                }
            } else {
                if($('#sidebar-left').css('display') != 'none') {
                    $('#sidebar-left').hide();
                    $('#mobile-menu-button').html('menu');
                }
            }
            $('.nav-menu').hide();
            $('.manage-menu').hide();
        }
    });
    // Fix height of non-scrolling pages if announcement and/or status message is visible
    if($('#announcement').length || $('.success, .errorm').length) {
        setTimeout(function() {
            const fix_height = 50 + ($('#announcement').length ? $('#announcement').outerHeight() : 0) + ($('.success, .errorm').length ? $('.success, .errorm').outerHeight() : 0);
            $('#wrapper').attr('style', 'min-height: calc(100dvh - ' + fix_height + 'px);');
        }, 25);
    }
    // Toggle Other Systems
    $('#toggle-other').click(function() {
        const target = $('#other-systems');
        if(target.is(':visible')) {
            $(this).html('arrow_right');
            target.slideUp('fast');
        } else {
            $(this).html('arrow_drop_down');
            target.slideDown('fast');
        }
        return false;
    });
    // Un-hide NSFW content
    $('#nsfw-warning').click(function() {
        $(this).hide();
        $('.nsfw:not(.no-zoom)').removeClass('nsfw');
        getScale();
    });
    // Remove borders from links on images
    $('img').parent('a').css('border', 'none');
    // Disable submit buttons
    $('form').submit(function() {
        $(this).find('input[type="submit"]').not('[name]').prop('disabled', true);
    });
    // Spoilers
    $('.spoiler').hover(function() {
        $(this).find('.spoiler-text').css('background-color', 'var(--bg-spoiler)');
    }, function () {
        $(this).find('.spoiler-text').css('background-color', 'var(--black)');
    });
    // Filter dropdowns
    let minh = 0;
    let dd = null;
    $('form').on('keyup click', '.dd-filter', function() {
        const textbox = $(this);
        dd = '#' + $(this).data('target');
        let count = $(dd + ' option').length;
        const optg = $(dd + ' optgroup').length;
        minh = !minh ? $(dd).css('height') : minh;
        $(dd).on('change', function() {
            $(this).removeAttr('size').css('min-height', '').css('max-height', '').css('height', '');
            $(dd + ' option').prop('hidden', false).prop('disabled', false);
            textbox.val('');
            $('input[data-target="' + $(this).attr('id') + '"]').focus();
        });
        $(dd + ' option').prop('hidden', true).prop('disabled', true);
        if(dd == '#timezone') {
            $(dd + ' option' + ':filter(' + $(this).val().replace(/ /g, '_') + ')').prop('hidden', false).prop('disabled', false);
        } else {
            $(dd + ' option' + ':filter(' + $(this).val() + ')').prop('hidden', false).prop('disabled', false);
        }
        count = $(dd + ' option:enabled').length;
        $(dd).attr('size', count + optg).css('min-height', minh).css('max-height', '100px').css('height', 'auto');
        if(count == 1) {
            $(dd + ' option').prop('selected', false);
            $(dd + ' option:enabled').prop('selected', true);
            $(this).val('');
            $(dd).trigger('change');
            $(this).blur();
        } else if(count == 0) {
            if($(this).val() != '') {
                $(this).val($(this).val().slice(0, -1));
                $(this).trigger('keyup');
            }
        }
    }).on('keydown', '.dd-filter', function(e) {
        const key = e.key.toLowerCase();
        if(key == 'enter' || key == 'escape' || key == 'tab') {
            $(document).trigger('click');
            if(key != 'tab') {
                $(dd).focus();
                return false;
            } else {
                $(this).val('');
            }
        }
    });
    // Auto-Select radio buttons and checkboxes on forms
    $('input[type!="radio"][type!="checkbox"]').change(function() {
        if($(this).prevAll('input[type="radio"], input[type="checkbox"]').length) {
            if($(this).val() != '') {
                $(this).prevAll('input[type="radio"]:first, input[type="checkbox"]:first').prop('checked', true);
            } else {
                $(this).prevAll('input[type="checkbox"]:first').prop('checked', false);
            }
        }
    });
    // Auto-check parent reasons and prevent un-checking them
    $('input[type="checkbox"][name="reason[]"]').change(function() {
        const parent = $(this).data('parent') == 0 ? 0 : $('#' + $(this).data('parent'));
        if(parent == 0) {
            const key = $(this).attr('id');
            let checked = false;
            $('input[type="checkbox"][data-parent="' + key + '"]').each(function() {
                if($(this).is(':checked')) {
                    checked = true;
                    return;
                }
            });
            if(checked && !$(this).is(':checked')) {
                $(this).prop('checked', true);
            }
        } else {
            if($(this).is(':checked') && !parent.is(':checked')) {
                parent.prop('checked', true);
            }
        }
    });
    // Auto-check delete when marking submission as spam
    $('input[type="checkbox"][id^="reason_999_"], #batch-spam').change(function() {
        if($(this).is(':checked') && !confirm('You have selected the "Spam" rejection checkbox. This will cause the submission(s) to be deleted and a permanent mark to be applied to the user\'s account. Click OK to confirm this action.')) {
            $(this).prop('checked', false);
            return false;
        }
        if($(this).attr('id') != 'batch-spam') {
            const id = $(this).attr('id').split('_')[2];
            if($(this).is(':checked')) {
                $('#delete-pending_' + id).prop('checked', true);
            }
        }
    });
    // Prevent un-checking delete if submission is marked as spam
    $('input[type="checkbox"][id^="delete-pending_"]').change(function() {
        const id = $(this).attr('id').split('_')[1];
        if($('#reason_999_' + id).is(':checked')) {
            $(this).prop('checked', true);
        }
    });
    // Show user controls
    let target = '';
    let offset = '';
    let visible = false;
    $('.menu-toggle').click(function(e) {
        target = '#manage-' + $(this).data('id');
        offset = $(this).offset();
        visible = $(target).is(':visible');
        $('.manage-menu').slideUp('fast');
        if(!visible) {
            $(target).css('top', offset.top + $(this).height() - 45 - ($('#announcement').length ? $('#announcement').outerHeight() : 0) - ($('.success, .errorm').length ? $('.success, .errorm').outerHeight() : 0) + 'px');
            $(target).css('right', $(window).width() - offset.left - $(this).outerWidth() + 'px');
            $(target).slideDown('fast');
        }
    });
    // Hide menus on click
    $(document).click(function(e) {
        if($('.nav-menu').is(':visible') && !$(e.target).closest('.nav-menu').length && !$(e.target).hasClass('nav-menu-button')) {
            $('.nav-menu').slideUp('fast');
        }
        if($('.manage-menu').is(':visible') && !$(e.target).closest('.manage-menu').length && !$(e.target).hasClass('menu-toggle')) {
            $('.manage-menu').slideUp('fast');
        }
        if($(window).outerWidth() <= mobile_width && $('#nav-links').is(':visible') && !$(e.target).closest('#nav-links').length && !$(e.target).is('#mobile-menu-button')) {
            $('#mobile-menu-button').html('menu');
            $('#nav-links').slideUp('fast');
        }
        if(dd !== null && !$(e.target).hasClass('dd-filter') && $(e.target).attr('id') != dd.substring(1)) {
            $(dd).removeAttr('size').css('min-height', '').css('max-height', '').css('height', '');
        }
    });
    // Clear filters
    $('#clearfilter').click(function() {
        window.location.href = page.replace(/\/page-[0-9]+/i, '');
    });
    // Confirm delete and logout
    $('a[href*="/logout"], .delete').click(function() {
        return confirm('Are you sure? Click OK to continue or Cancel to go back.');
    });
    // Generic link buttons
    $('.linkbutton').click(function() {
        window.location.href = $(this).data('href');
    });
    // Collapsable controls
    $('.collapse-toggle').click(function() {
        const target = $(this).parents().children('.collapse');
        if(target.is(':visible')) {
            $(this).html('arrow_right');
            target.slideUp('fast');
        } else {
            $(this).html('arrow_drop_down');
            target.slideDown('fast');
        }
    });
    // Section toggle
    $('#asset-display, #assetdisplay, #update-assets').on('click', '.section', function() {
        const target = $(this).next('.icondisplay').length ? $(this).next('.icondisplay') : $(this).next().next('.icondisplay');
        if(!target.hasClass('icondisplay')) {
            return;
        }
        const arrow = $(this).children('.section-toggle');
        if(target.is(':visible')) {
            arrow.html('arrow_right');
            target.slideUp('fast');
        } else {
            arrow.html('arrow_drop_down');
            target.slideDown('fast');
        }
    });
    // Section toggle (all)
    $('#content').on('click', '#section-toggle-all', function() {
        if($(this).html() == 'arrow_drop_down') {
            $(this).html('arrow_right');
            $('.section-toggle').html('arrow_right');
            $('.icondisplay').slideUp('fast');
        } else {
            $(this).html('arrow_drop_down');
            $('.section-toggle').html('arrow_drop_down');
            $('.icondisplay').slideDown('fast');
        }
    });
    // Text entry previews
    $('#preview').click(function() {
        const button = $(this);
        const field = button.parents('form').find('textarea');
        const text = field.val();
        if(text) {
            if($('#preview-display').is(':visible')) {
                $('#preview-display').hide();
                button.parents('form').find('textarea').fadeIn('fast');
                button.val('Preview');
            } else {
                $.ajax({
                    url: '/process/preview/',
                    method: 'POST',
                    data: {
                        text: text,
                        type: field.attr('name')
                    },
                    success: function(response) {
                        $('#preview-display').html(response);
                        field.hide();
                        $('#preview-display').fadeIn('fast');
                        button.val('Return');
                    },
                    error: function(xhr, status, error) {
                        alert('Error previewing input. Please try again.');
                    }
                });
            }
        }
    });
    // Tag filter debounce
    let timeout = null;
    function filter(input) {
        clearTimeout(timeout);
        timeout = setTimeout(function(input) {
            $.ajax({
                url: '/process/tagsearch/',
                method: 'POST',
                data: {
                    name: input.closest('.filter-parent').find('input[type="text"]').val(),
                    cat: input.closest('.filter-parent').find('select').length ? input.closest('.filter-parent').find('select').val() : (input.closest('.filter-parent').data('cat') ? input.closest('.filter-parent').data('cat') : ''),
                    tcn: input.closest('.filter-parent').data('tcn'),
                    check_name: input.closest('.filter-parent').data('check-name'),
                    check_id: input.closest('.filter-parent').data('check-id')
                },
                success: function(response) {
                    input.closest('.filter-parent').find('.filter-options').html(response);
                },
                error: function(xhr, status, error) {
                    alert('Error searching tags. Please try again.');
                }
            });
        }, 500, input);
    }
    // Tag filter
    $('[id^="filter-"]').on('keyup change', function(e) {
        if(($(this).is('select') && e.type == 'change') || e.type == 'keyup') {
            filter($(this));
        }
    });
    // Tag selection processing
    $('.filter-parent').on('click', '.filter-item label', function(e) {
        const id = $(this).attr('for') || $(this).data('checkid');
        const sel = $(this).closest('.filter-parent').data('target') ? $('#' + $(this).closest('.filter-parent').data('target')) : $(this).closest('.filter-parent').find('.filter-selected');
        const tagid = $(this).data('tagid') ? $(this).data('tagid') : '';
        const catid = $(this).data('catid') !== '' ? $(this).data('catid') : '';
        const cname = $(this).data('check-name') ? $(this).data('check-name') : '';
        const label_text = $(this).parents('.filter-options').data('label');
        const label = label_text ? '<span class="browse-tag-catname">' + label_text + '</span> ' : '';
        if(!id || !tagid || catid === '' || !cname) {
            alert('Error processing tag - please refresh and try again.');
            return;
        }
        if(!sel.find('label[for="' + id + '"]').length) {
            const cat = $(this).find('span').text();
            const tag = $(this).html().replace(/<span.+\/span>/, '').trim();
            const title = (label_text && $(this).parent().not('.filter-container') ? label_text + ': ' : (cat ? cat + ': ' : '')) + tag;
            sel.append('<span class="filter-item"><input type="checkbox" name="' + cname + '" id="' + id + '" value="' + tagid + '" checked><label for="' + id + '" title="' + title + '"><span class="material-symbols-outlined">disabled_by_default</span> ' + label + $(this).html() + '</label></span>\n');
            $(this).addClass('selected');
            e.preventDefault();
        } else {
            sel.find('label[for="' + id + '"]').parent().remove();
            $(this).removeClass('selected');
        }
    });
    // Unselect tag
    $('.filter-selected').on('click', 'label', function() {
        $(this).parent().remove();
    });
    // Disable enter key in tag filter
    $('input[type="text"][id^="filter-"]').keydown(function(e) {
        if(e.key.toLowerCase() == 'enter') {
            e.preventDefault();
            return false;
        }
    });
    // Clear tag filter
    $('.filter-parent').on('click', 'input.filter-clear', function() {
        $(this).closest('.filter-parent').find('input[type="text"][id^="filter-"]').val('');
        $(this).closest('.filter-parent').find('select[id^="filter-"]').val($(this).closest('.filter-parent').find('select[id^="filter-"] option').first().val());
        $(this).closest('.filter-parent').find('.filter-options').html('');
        return false;
    });
    // New tag button
    $('button.newtag').click(function() {
        const parent = $(this).closest('.filter-parent');
        const textbox = parent.find('input[type="text"]');
        const dropdown = parent.find('select');
        const cname = parent.data('check-name');
        const newname = $(this).data('check-name');
        const newid = $(this).data('check-id');
        if(!parent.length || !textbox.length || textbox.val() == '' || textbox.val().toLowerCase() == 'untagged' || !newname || !cname) {
            return;
        }
        $.ajax({
            url: '/process/newtag/',
            method: 'POST',
            data: {
                text: textbox.val(),
                cat: dropdown.val()
            },
            success: function(response) {
                if(response) {
                    const json = $.parseJSON(response);
                    const cat = json.cat_id ? ('<span class="browse-tag-catname">' + json.cat_name + '</span> ') : '';
                    const cat_id = json.cat_id ? ('||' + json.cat_id) : '';
                    const existing = parent.find('input[id^="new-"][id$="-' + json.id + '-' + json.cat_id + '"]');
                    if(existing.length) {
                        existing.parent().remove();
                    }
                    parent.find('.filter-selected').append('<span class="filter-item"><input type="checkbox" name="' + newname + '" id="' + newid + '-' + json.id + '-' + json.cat_id + '" value="' + json.name + cat_id + '" checked><label for="' + newid + '-' + json.id + '-' + json.cat_id + '"><span class="material-symbols-outlined">disabled_by_default</span> ' + cat + json.name + '</label></span>\n');
                }
                textbox.val('');
            },
            error: function(xhr, status, error) {
                alert('Error creating new tag. Please try again.');
            }
        });
    });
    // Browse filter controls
    $('#browse-choice-box div[id^="toggle_"]').click(function() {
        const target = $('#' + $(this).attr('id').split('_')[1]);
        $('#browse-choice-box div[id^="toggle_"] span').html('arrow_right');
        $('#browse-choice-box div[id^="section-"]').slideUp('fast');
        if(!target.is(':visible')) {
            target.slideDown('fast');
            $(this).children('span').html('arrow_drop_down');
        }
    });
    // Tag filter warning
    $('form#tag-filter').submit(function() {
        const name = $(this).find('input[name="name"]').val().trim();
        const cat = $(this).find('select[name="cat"]').val();
        const unused = $(this).find('input[name="unused').is(':checked');
        const caps = $(this).find('input[name="caps').is(':checked');
        if(!name && !cat && !unused && !caps) {
            $(this).find('input[type="submit"]').prop('disabled', false);
            return confirm('All tags will be displayed. Are you sure? Click OK to continue or Cancel to return.');
        }
    });
    // Tag category filter debounce (staff)
    function filterTags(input) {
        clearTimeout(timeout);
        timeout = setTimeout(function(input) {
            const on = input.attr('id');
            let search = input.val().replace('"', '\\"');
            search = !input.is(':checkbox') || input.is(':checked') ? search : '';
            let modifier = input.is('select') ? '=' : '*=';
            const query = search != '' ? ('[data-' + on + modifier + '"' + search + '" i]') : ('[data-' + on + ']');
            $('#taglist tr:not(.rowheader)').hide();
            $('.' + on + query).parent().css('display', 'table-row');
            let me = '';
            $('.tag-filter').each(function() {
                me = $(this).attr('id');
                search = $(this).val().replace('"', '\\"');
                search = !$(this).is(':checkbox') || $(this).is(':checked') ? search : '';
                modifier = $(this).is('select') ? '=' : '*=';
                if(search != '') {
                    $('.' + me + ':not([data-' + me + modifier + '"' + search + '" i])').filter(':visible').parent().hide();
                }
            });
            $('#taglist tr:not(.rowheader):visible:even').removeClass().addClass('even');
            $('#taglist tr:not(.rowheader):visible:odd').removeClass().addClass('odd');
            const num = $('#taglist tr:not(.rowheader):visible').length;
            $('#tag_count').html(num.toLocaleString());
        }, 500, input);
    }
    // Tag category filter (staff)
    $('.tag-filter').on('keyup change', function(e) {
        if(($(this).is('select, input[type="checkbox"]') && e.type == 'change') || e.type == 'keyup') {
            filterTags($(this));
        }
    });
    // Clear tag filter (staff)
    $('#clear-filter').click(function() {
        $('.tag-filter').each(function() {
            if($(this).is(':checkbox')) {
                $(this).prop('checked', false);
            } else {
                $(this).val('');
            }
        });
        $('.tag-filter').first().trigger('keyup');
    });
    // Highlight tag delete
    $('#managetags, #managetagcats').on('change', 'input[name="delete[]"]', function() {
        let target = $(this).parents('tr');
        if($(this).is(':checked')) {
            target.addClass('delete');
        } else {
            target.removeClass('delete');
        }
    });
    // Confirm tag delete
    $('#submit_tags').click(function() {
        if($('input[name="delete[]"]:checked').length) {
            return confirm('You have marked one or more tags for deletion. Click OK to continue or Cancel to return.');
        }
    });
    // Tag management client-side verification
    $('#managetags, #managetagcats').submit(function() {
        $(this).find('tr').each(function() {
            $(this).find('input[type!="checkbox"], select').each(function() {
                if($(this).val() == $(this).data('current')) {
                    $(this).prop('disabled', true);
                }
            });
            if($(this).find('input:disabled, select:disabled').length < 2 || $(this).find('input[type="checkbox"]:checked').length) {
                $(this).find('input, select').prop('disabled', '');
            }
        });
        const changed_fields = $(this).find('input[type!="checkbox"], select').not(':disabled').length;
        const checked_delete = $(this).find('input[type="checkbox"]:checked').length;
        if(changed_fields + checked_delete > 950) {
            alert('A maximum of 950 form fields can be submitted at once. Please reduce your number of changes and try again.');
            return false;
        }
    });
    // Show hidden tags
    $('#showtags').click(function() {
        if($('#moretags').is(':visible')) {
            $(this).html('arrow_right');
            $('#moretags').slideUp('fast');
        } else {
            $(this).html('arrow_drop_down');
            $('#moretags').slideDown('fast');
        }
    });
    // Registration token protection
    $('#regcontainer').ready(function() {
        $('#regcontainer').html('<span class="revealtoken">(Click here to reveal registration token)</span>');
        if(window.location.hash == '#regcontainer') {
            $('#regcontainer').addClass('highlight');
        }
    });
    // Show token
    $('#regcontainer').on('click', 'span.revealtoken', function() {
        const fparent = $(this).parent();
        fparent.hide();
        $.ajax({
            url: '/process/user/showtoken/',
            method: 'POST',
            success: function(response) {
                fparent.html(response);
                fparent.fadeIn('fast');
            },
            error: function(xhr, status, error) {
                fparent.html('Error showing registration token. Please try again.');
                fparent.fadeIn('fast');
            }
        });
    });
    // Show API key
    $('#api_key_display').on('click', 'span', function() {
        const pw = $('#currentpw').val();
        if(!pw) {
            alert('Current Password is required for this action.');
            return;
        }
        const td = $(this).parent();
        $.ajax({
            url: '/process/user/showkey/',
            method: 'POST',
            data: {
                password: pw,
                session_key: getSession()
            },
            success: function(response) {
                td.html(response);
            },
            error: function(xhr, status, error) {
                td.html('Error showing API key. Please try again.');
            }
        });
        setTimeout(function() {
            $('#session_key').val(getSession());
        }, 1000);
    });
    // Regenerate API key
    $('#api_key_regen').click(function() {
        const pw = $('#currentpw').val();
        if(!pw) {
            alert('Current Password is required for this action.');
            return;
        }
        if(confirm('Are you sure? Your old key will be immediately invalidated and the new one will be displayed above. Click OK to proceed.')) {
            const td = $('#api_key_display');
            $.ajax({
                url: '/process/user/genapikey/',
                method: 'POST',
                data: {
                    password: pw,
                    session_key: getSession()
                },
                success: function(response) {
                    td.html(response);
                },
                error: function(xhr, status, error) {
                    td.html('Error creating new API key. Please try again.');
                }
            });
            setTimeout(function() {
                $('#session_key').val(getSession());
            }, 1000);
        }
    });
    // Comment reply
    $('div[id^="setreply_"]').click(function() {
        const id = $(this).attr('id').split('_')[1];
        const name = $(this).data('author');
        const pid = $(this).data('pid');
        if(!$(this).hasClass('selected')) {
            $(this).addClass('selected');
            $('#comment_replyto_' + pid).val(id);
            $('#replyto_name_' + pid).html(name);
            $('#replyto_cancel_' + pid).data('cid', id);
            $('#replyto_container_' + pid).slideDown('fast');
            $('#comment_text_' + pid).focus();
        } else {
            $(this).removeClass('selected');
            $('#comment_replyto_' + pid).val('');
            $('#replyto_container_' + pid).slideUp('fast');
        }
        $('div[id^="setreply_"]').not(this).removeClass('selected');
    });
    // Cancel reply
    $('span[id^="replyto_cancel_"]').click(function() {
        const id = $(this).data('cid');
        $('#setreply_' + id).click();
    });
    // "Clear" checkbox warnings
    $('input[type="checkbox"][id^="clear_"]').change(function() {
        if($(this).is(':checked')) {
            if(!confirm('Are you sure? Click OK to confirm.')) {
                $(this).prop('checked', false);
            }
        }
    });
    // Highlight selected comment
    let hash = window.location.hash.substring(1);
    let hash_parts = hash.split('_');
    if(hash_parts.length == 2 && hash_parts[0] == 'comment' && $.isNumeric(hash_parts[1])) {
        $('#comment_' + hash_parts[1] + ' .commentbox').addClass('commenthighlight');
    }
    $(window).on('hashchange', function() {
        hash = window.location.hash.substring(1);
        hash_parts = hash.split('_');
        if(hash_parts.length == 2 && hash_parts[0] == 'comment' && $.isNumeric(hash_parts[1])) {
            $('.commentbox').removeClass('commenthighlight');
            $('#comment_' + hash_parts[1] + ' .commentbox').addClass('commenthighlight');
        }
    });
    // Expand IPv6 addresses
    $('.ipaddr').on('click', '.iptoggle', function() {
        const parent = $(this).parent();
        if($(this).text() == '...') {
            const short = parent.html();
            parent.html($(this).data('long') + ' <span class="iptoggle material-symbols-outlined">disabled_by_default</span>');
            parent.children('span').data('short', short);
        } else {
            parent.html($(this).data('short'));
        }
        return false;
    });
    // Character counters
    const show_count = function() {
        let len = new TextEncoder().encode($(this).val() || '').length;
        len += ($(this).val().match(/\r\n|\n|\r/g) || []).length;
        if($(this).attr('maxlength')) {
            $('#showcount_' + $(this).attr('id')).html('(' + len.toLocaleString() + ' / ' + parseInt($(this).attr('maxlength'), 10).toLocaleString() + ')');
        } else {
            $('#showcount_' + $(this).attr('id')).html('(' + len.toLocaleString() + ')');
        }
    }
    $('.showcount').each(show_count).on('change keyup', show_count);
    // Password checks
    $('#password, #confirm').on('change keyup', function() {
        $('#password, #confirm').css('color', 'var(--' + (($('#password').val() && $('#confirm').val() ? ($('#password').val() === $('#confirm').val() ? 'bg-success' : 'text-error') : '')) + ')');
    });
    // Delete notification
    $('div.notification-close').click(function() {
        const target = $(this).parent();
        const id = target.attr('id').split('-')[1];
        $.ajax({
            url: '/process/deletenotification/',
            method: 'POST',
            data: {
                id: id,
                session_key: getSession()
            },
            success: function(response) {
                const json = $.parseJSON(response);
                let nc = 0;
                target.slideUp('fast');
                for(let i = 1; i <= 6; i++) {
                    if(typeof json[i] == 'undefined') {
                        if($('#notifications-container-' + i).is(':visible')) {
                            $('#notifications-container-' + i).slideUp('fast');
                        }
                    } else {
                        nc += json[i].length;
                    }
                }
                $('#notification-count').html(nc ? (nc <= 999 ? nc : '...') : '');
                if(!nc) {
                    $('.instructions').hide();
                    $('#nav-notifications').html('').addClass('zero');
                    $('#notifications-none').slideDown('fast');
                }
            },
            error: function(xhr, status, error) {
                alert('Error deleting notification. Please try again.');
            }
        });
    });
    // Delete all notifications
    $('#notification-close-all').click(function() {
        if(!confirm('Are you sure? Click OK to confirm.')) {
            return;
        }
        $.ajax({
            url: '/process/deletenotification/',
            method: 'POST',
            data: {
                session_key: getSession()
            },
            success: function() {
                $('.instructions').hide();
                $('div[id^="notifications-container-"]').slideUp('fast');
                $('#nav-notifications').html('').addClass('zero');
                $('#notifications-none').slideDown('fast');
            },
            error: function(xhr, status, error) {
                alert('Error deleting all notifications. Please try again.');
            }
        });
    });
    // Pan and zoom controls
    if($('#zoomdisplay').length) {
        function gridPos(val) {
            return window.devicePixelRatio == 1 ? Math.round(val) : Math.floor(val);
        }
        function getScale() {
            scale = !$('#assetdisplay > img').length || !$('#assetdisplay > img').width() || $('#assetdisplay > img').width() < $('#assetdisplay > img').prop('naturalWidth') ? 0 : 1;
            $('#nottoscale').css('display', $('#assetdisplay > img').length && scale < 1 ? 'block' : '');
        }
        let p0 = { x: null, y: null };
        let dragging = false;
        let scale = 0;
        let flip = 1;
        let rotate = 0;
        // Get initial scale of image
        $('#assetdisplay > img').on('load', function() {
            getScale();
            $('#mag').text(scale);
            $('#assetdisplay > img').css('cursor', 'zoom-in');
        }).each(function() {
            $(this).on('click', function(e) {
                const x = e.offsetX / $(this).width();
                const y = e.offsetY / $(this).height();
                $('#zoom-in').click();
                $('#zoomdisplay > img').css('top', Math.round($('#zoomdisplay > img').height() / 2 - $('#zoomdisplay > img').height() * y));
                $('#zoomdisplay > img').css('left', Math.round($('#zoomdisplay > img').width() / 2 - $('#zoomdisplay > img').width() * x));
                $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
                $('#zoomdisplay').append('<div id="crosshair"><div id="crosshair-x"></div><div id="crosshair-y"></div><div id="crosshair-center"></div></div>\n');
                setTimeout(function() {
                    $('#crosshair').fadeOut('slow', function() {
                        $('#crosshair').remove();
                    });
                }, 500);
            });
            if(this.complete) {
                $(this).trigger('load');
            }
        });
        // Pan
        $('html').on('mousedown touchstart', function(e) {
            if($('#zoomdisplay > img').length && $('#zoomdisplay').hasClass('zoomed') && ($(e.target).attr('id') == 'zoomdisplay' || $(e.target).parent().attr('id') == 'zoomdisplay')) {
                e.preventDefault();
                dragging = true;
                p0 = { x: (e.pageX || (e.touches && e.touches[0].clientX)) - $('#zoomdisplay').offset().left, y: (e.pageY || (e.touches && e.touches[0].clientY)) - $('#zoomdisplay').offset().top };
                $('html').css('user-select', 'none').css('--webkit-user-select', 'none').css('cursor', 'move');
                $('*').css('pointer-events', 'none');
            }
        }).on('mousemove touchmove', function(e) {
            if(dragging) {
                const p1 = { x: (e.pageX || (e.touches && e.touches[0].clientX)) - $('#zoomdisplay').offset().left, y: (e.pageY || (e.touches && e.touches[0].clientY)) - $('#zoomdisplay').offset().top };
                const cx = p1.x - p0.x;
                const cy = p1.y - p0.y;
                p0 = p1;
                $('#zoomdisplay > img').css('top', Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) + cy) + 'px');
                $('#zoomdisplay > img').css('left', Math.round(parseInt($('#zoomdisplay > img').css('left'), 10) + cx) + 'px');
                $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
            }
        }).on('mouseup touchend', function(e) {
            dragging = false;
            $('html').css('user-select', '').css('--webkit-user-select', '').css('cursor', '');
            $('*').css('pointer-events', '');
        });
        // Fix click on drag release in Firefox
        $('#zoomdisplay').on('mousedown', 'canvas', function() {
            $('html').css('user-select', 'none').css('--webkit-user-select', 'none');
            $('*').css('pointer-events', 'none');
        }).on('mouseup', 'canvas', function(e) {
            $('html').css('user-select', '').css('--webkit-user-select', '');
            $('*').css('pointer-events', '');
        });
        // Zoom
        $('#zoom-in, #zoom-out, #zoom-3d').click(function() {
            $(this).blur();
            $('body').css('overflow', 'hidden');
            $('#zoomdisplay, #zoom-controls').addClass('zoomed');
            if($('#zoomdisplay > img').length) {
                const dir = $(this).attr('id') == 'zoom-in' ? 1 : -1;
                scale = scale + dir;
                if(scale < 1) {
                    scale = 1;
                    return;
                }
                const mult = dir == 1 ? (scale / (scale - 1)) : (1 / ((scale + 1) / scale));
                $('#mag').text(scale);
                $('#zoomdisplay > img').width($('#zoomdisplay > img').prop('naturalWidth') * scale);
                $('#zoomdisplay > img').height($('#zoomdisplay > img').prop('naturalHeight') * scale);
                $('#zoomdisplay > img').css('top', Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) * mult) + 'px');
                $('#zoomdisplay > img').css('left', Math.round(parseInt($('#zoomdisplay > img').css('left'), 10) * mult) + 'px');
                $('#pixel-grid').width(Math.abs(rotate) % 2 == 0 ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height());
                $('#pixel-grid').height(Math.abs(rotate) % 2 == 0 ? $('#zoomdisplay > img').height() : $('#zoomdisplay > img').width());
                $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
                $('#pixel-grid').css('background-size', scale + 'px ' + scale + 'px');
                $('#zoom-in').prop('title', 'Zoom In (Mouse: Scroll Up, Keyboard: Up Arrow)');
                $('#zoom-out').prop('disabled', scale == 1 ? true : false);
            } else {
                loadModel($(this).data('path'), $(this).data('tf'));
                $('#zoomdisplay').css('background-size', 'var(--preview-bg-size)');
                $('#zoomdisplay').css('background-position', 'var(--preview-bg-position)');
                $('#zoomdisplay').css('background-image', 'var(--preview-bg-pattern)');
                $('#zoomdisplay').css('background-color', 'var(--bg-icon-base)');
            }
            $('#footer').css('display', 'none');
            // Advertiser-Specific - close anchor if visible
            if($('#nitro-anchor').length) {
                $('#nitro-anchor-close').click();
                $('#nitro-anchor').remove();
            }
        });
        // Zoom reset (close)
        $('#zoom-reset').click(function() {
            $(this).blur();
            $('body').css('overflow', '');
            if($('#pixel-grid').length && $('#pixel-grid').is(':visible')) {
                $('#pixel-grid').css('display', '');
                $('#pixel-grid').removeClass();
                $('#zoom-grid').addClass('off');
            }
            $('#zoomdisplay, #zoom-controls').removeClass('zoomed');
            if($('#zoomdisplay > img').length) {
                if($('#zoomdisplay > img#gif').length) {
                    pauseGif();
                    $('#zoomdisplay > img').removeAttr('id');
                    $('#zoom-controls-gifs-bar, #zoom-controls-gifs-info, #zoom-controls-gifs-buttons, #gifs-submitter').remove();
                }
                if(!$('#assetdisplay .category-container').length) {
                    $('#zoomdisplay > img').attr('src', $('#assetdisplay > img').attr('src'));
                    $('#zoomdisplay > img, #pixel-grid').css('width', '');
                    $('#zoomdisplay > img, #pixel-grid').css('height', '');
                    $('#zoomdisplay > img, #pixel-grid').css('top', '');
                    $('#zoomdisplay > img, #pixel-grid').css('left', '');
                    $('#zoomdisplay > img, #pixel-grid').css('transform', '');
                    $('#pixel-grid').css('background-size', '1px 1px');
                    $('#zoomdisplay, #zoomcontrols').css('width', '');
                    $('#zoom-in').prop('title', 'Zoom In');
                } else {
                    $('#zoomdisplay').html('');
                    $('#zoom-in').addClass('extra');
                    $('#zoom-previous, #zoom-next').prop('disabled', false);
                }
                getScale();
                $('#download').attr('href', $('#download').data('file')).attr('download', $('#download').data('download'));
                $('#zoom-flip').addClass('off');
                flip = 1;
                rotate = 0;
                $('#mag').text(scale);
            } else {
                if(!$('#zoom-turn').hasClass('off')) {
                    toggleTurntable();
                }
                if(!$('#zoom-wire').hasClass('off')) {
                    toggleWireframe();
                }
                if($('#zoom-3d').data('tf') == true) {
                    $('#zoom-tf').addClass('off');
                } else {
                    $('#zoom-tf').removeClass('off');
                }
                $('#zoom-turn, #zoom-wire, #zoom-bg').addClass('off');
                $('#zoomdisplay').css('background-size', '');
                $('#zoomdisplay').css('background-position', '');
                $('#zoomdisplay').css('background-image', '');
                $('#zoomdisplay').css('background-color', '');
                clearInterval(window.animationInterval);
                window.animationInterval = null;
            }
            $('#footer').css('display', '');
        });
        // Zoom reset (1x)
        $('#zoom-info').click(function() {
            $(this).blur();
            $('#zoomdisplay > img, #pixel-grid').width($('#zoomdisplay > img').prop('naturalWidth'));
            $('#zoomdisplay > img, #pixel-grid').height($('#zoomdisplay > img').prop('naturalHeight'));
            $('#zoomdisplay > img').css('top', '');
            $('#zoomdisplay > img').css('left', '');
            $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
            $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
            $('#zoomdisplay > img').css('transform', '');
            $('#zoom-flip').addClass('off');
            $('#pixel-grid').css('background-size', '1px 1px');
            scale = 1;
            flip = 1;
            rotate = 0;
            $('#mag').text(scale);
            $('#zoom-out').prop('disabled', true);
            if($('#zoomdisplay > img#gif').length) {
                loadGif();
            }
        });
        // Rotate
        $('#zoom-rotate').click(function() {
            $(this).blur();
            rotate = rotate + 1;
            if(rotate > 3) {
                rotate = 0;
            }
            const top = Math.round(parseInt($('#zoomdisplay > img').css('left'), 10));
            const left = Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) * -1);
            $('#zoomdisplay > img').css('transform', 'rotate(' + (rotate * 90) + 'deg) scale(' + flip + ', 1)');
            $('#zoomdisplay > img').css('top', top + 'px');
            $('#zoomdisplay > img').css('left', left + 'px');
            $('#pixel-grid').width(rotate % 2 == 0 ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height());
            $('#pixel-grid').height(rotate % 2 == 0 ? $('#zoomdisplay > img').height() : $('#zoomdisplay > img').width());
            $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
            $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
        });
        // Toggle horizontal flip
        $('#zoom-flip').click(function() {
            $(this).blur();
            flip = flip * -1;
            const side = rotate % 2 == 0 ? 'left' : 'top';
            $('#zoomdisplay > img').css('transform', 'rotate(' + (rotate * 90) + 'deg) scale(' + flip + ', 1)');
            $('#zoomdisplay > img').css(side, Math.round(parseInt($('#zoomdisplay > img').css(side), 10) * -1) + 'px');
            $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
            $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
            if(flip == -1) {
                $(this).removeClass('off');
            } else {
                $(this).addClass('off');
            }
        });
        // Toggle pixel grid
        let currgrid = 0;
        let newgrid = 0;
        $('#zoom-grid').on('mousedown', function(e) {
            $(this).blur();
            newgrid = e.which ? e.which : 1;
            newgrid = newgrid == 2 ? 1 : newgrid;
            if($(this).hasClass('off') || newgrid != currgrid) {
                if(newgrid == '3') { // Right click
                    $('#pixel-grid').addClass('white');
                } else {
                    $('#pixel-grid').removeClass();
                }
                $('#pixel-grid').css('display', 'block');
                $(this).removeClass('off');
                currgrid = newgrid;
            } else {
                $('#pixel-grid').css('display', '');
                $('#pixel-grid').removeClass();
                $(this).addClass('off');
            }
        }).on('contextmenu', function(e) {
            e.preventDefault();
            return false;
        });
        // Display preview model on preview icon click
        $('div.iconcontainer.preview').click(function() {
            $(this).blur();
            if($('#zoom-3d').length) {
                $('#zoom-3d').click();
                return false;
            }
        });
        // Toggle turntable
        $('#zoom-turn').click(function() {
            $(this).blur();
            toggleTurntable();
            if($(this).hasClass('off')) {
                $(this).removeClass('off');
            } else {
                $(this).addClass('off');
            }
        });
        // Toggle wireframe
        $('#zoom-wire').click(function() {
            $(this).blur();
            toggleWireframe();
            if($(this).hasClass('off')) {
                $(this).removeClass('off');
            } else {
                $(this).addClass('off');
            }
        });
        // Toggle background
        let currbg = 0;
        let newbg = 0;
        $('#zoom-bg').on('mousedown', function(e) {
            $(this).blur();
            let color = '#00ff00';
            newbg = e.which ? e.which : 1;
            switch(newbg) {
                default:
                case 1: // Left click
                    break;
                case 2: // Middle click
                    color = '#ff00ff';
                    break;
                case 3: // Right click
                    color = '#0000ff';
                    break;
            }
            if($(this).hasClass('off') || newbg != currbg) {
                $(this).removeClass('off');
                $('#zoomdisplay').css('background-size', '');
                $('#zoomdisplay').css('background-position', '');
                $('#zoomdisplay').css('background-image', '');
                $('#zoomdisplay').css('background-color', color);
                currbg = newbg;
            } else {
                $(this).addClass('off');
                $('#zoomdisplay').css('background-size', 'var(--preview-bg-size)');
                $('#zoomdisplay').css('background-position', 'var(--preview-bg-position)');
                $('#zoomdisplay').css('background-image', 'var(--preview-bg-pattern)');
                $('#zoomdisplay').css('background-color', 'var(--bg-icon-base)');
            }
        }).on('contextmenu', function(e) {
            e.preventDefault();
            return false;
        });
        // Toggle texture filtering
        $('#zoom-tf').click(function() {
            $(this).blur();
            if($(this).hasClass('off')) {
                $(this).removeClass('off');
                processTextures(false);
            } else {
                $(this).addClass('off');
                processTextures(true);
            }
        });
        // Reset camera
        $('#zoom-cam').click(function() {
            $(this).blur();
            resetCamera();
        });
        // Extract zip and display results
        $('#extract-zip').click(function() {
            $('#assetdisplay').html('<div class="loading-indicator">Downloading...</div>\n<div class="progress-bar-container"><div class="progress-bar"></div></div>\n');
            processAndDownload($(this).data('file'), $(this).data('type'));
            $(this).prop('disabled', true);
            $('h1:first').append('<span id="section-toggle-all" class="material-symbols-outlined arrow thin-stroke" title="Expand/Collapse All Sections">arrow_drop_down</span>\n');
            if($(this).data('type') == 'image') {
                $('#zoom-controls-buttons').prepend('<button id="zoom-previous" class="button thin-stroke extra" title="Previous Image (Keyboard: Left Arrow)"><span class="material-symbols-outlined">arrow_left</span></button>\n<button id="zoom-next" class="button thin-stroke extra" title="Next Image (Keyboard: Right Arrow)"><span class="material-symbols-outlined">arrow_right</span></button>\n');
            }
            // Advertiser-Specific - close anchor if visible
            if($('#nitro-anchor').length) {
                $('#nitro-anchor-close').click();
                $('#nitro-anchor').remove();
            }
        });
        // Zip image navigation and name toggle
        $('#zoom-controls').on('click', '#zoom-previous, #zoom-next', function() {
            $(this).blur();
            $('#zoom-previous, #zoom-next').prop('disabled', false);
            const dir = $(this).attr('id') == 'zoom-next' ? 1 : -1;
            const imgs = $('#assetdisplay .thumbnail img');
            const current = imgs.index($('#assetdisplay .thumbnail img[src="' + $('#zoomdisplay > img').attr('src') + '"]'));
            const next = current + dir;
            $('#zoomdisplay > img').attr('src', $(imgs[next]).attr('src'));
            $('#zoomdisplay > img').width($('#zoomdisplay > img').prop('naturalWidth') * scale);
            $('#zoomdisplay > img').height($('#zoomdisplay > img').prop('naturalHeight') * scale);
            $('#pixel-grid').width(Math.abs(rotate) % 2 == 0 ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height());
            $('#pixel-grid').height(Math.abs(rotate) % 2 == 0 ? $('#zoomdisplay > img').height() : $('#zoomdisplay > img').width());
            $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
            $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
            $('#download').attr('href', $(imgs[next]).attr('src')).attr('download', $(imgs[next]).attr('title'));
            if(next == 0) {
                $('#zoom-previous').prop('disabled', true);
            }
            if(next == imgs.length - 1) {
                $('#zoom-next').prop('disabled', true);
            }
        }).on('click', '#zoom-names', function() {
            $(this).blur();
            if($(this).hasClass('off')) {
                $(this).removeClass('off');
                $('.thumbnail-name').css('display', 'block');
            } else {
                $(this).addClass('off');
                $('.thumbnail-name').css('display', '');
            }
        });
        // Open zip image at center via file name
        $('#assetdisplay').on('click', '.thumbnail-name', function() {
            $(this).parent().find('img').trigger('click');
        });
        // Highlight current track when playing audio
        $('#assetdisplay').on('click', '.audio-container button', function() {
            $('#assetdisplay .audio-container > div').removeClass('highlight');
            $(this).parent().parent().addClass('highlight');
        });
        // Open animated GIFs in image viewer
        $('#gifs-body').on('click', '.iconcontainer', function() {
            const src = $(this).find('img').attr('src');
            if($('#zoomdisplay > img').length) {
                $('#zoomdisplay > img').attr('id', 'gif');
            } else {
                $('#zoomdisplay').html('<img id="gif" alt>\n<div id="pixel-grid"></div>\n');
                $('#zoom-info, #zoom-out, #zoom-in, #zoom-rotate, #zoom-flip, #zoom-grid, #zoom-close').removeClass('no-zoom');
                if($('#zoom-previous, #zoom-next').length) {
                    $('#zoom-previous, #zoom-next').addClass('no-zoom');
                }
            }
            $('#zoomdisplay > img').attr('src', '');
            $('#zoom-info').after('<div id="zoom-controls-gifs-buttons"><button id="zoom-gifs-play" class="button thin-stroke" title="Play/Pause (Keyboard: Space)"><span class="material-symbols-outlined">pause</span></button>\n<button id="zoom-gifs-back" class="button thin-stroke" title="Previous Frame (Keyboard: Left Arrow)"><span class="material-symbols-outlined">arrow_back</span></button>\n<button id="zoom-gifs-forward" class="button thin-stroke" title="Next Frame (Keyboard: Right Arrow)"><span class="material-symbols-outlined">arrow_forward</span></button>\n<button id="zoom-gifs-speed-down" class="button thin-stroke" title="Speed Down (Keyboard: Z)"><span class="material-symbols-outlined">timer_arrow_down</span></button>\n<button id="zoom-gifs-speed-up" class="button thin-stroke" title="Speed Up (Keyboard: X)"><span class="material-symbols-outlined">timer_arrow_up</span></button>\n<button id="zoom-gifs-pingpong" class="button thin-stroke off" title="Ping Pong (Keyboard: P)"><span class="material-symbols-outlined">arrow_range</span></button></div>').after('<div id="zoom-controls-gifs-info">Frame: <div id="zoom-gifs-curr"></div> / <div id="zoom-gifs-max"></div> | Speed: <div id="zoom-gifs-speed">1.00x</div></div>').after('<div id="zoom-controls-gifs-bar"><input type="range" id="zoom-gifs-seek" min="0" max="0" value="0"></div>');
            loadGif(src);
            scale = 0;
            $('#zoom-in').click();
            $('#zoomdisplay').prepend('<div id="gifs-submitter">Contributed by: ' + $(this).data('submitter') + '</div>');
            $('#download').attr('href', src).attr('download', $('#gifs-title').data('prefix') + ' - ' + $(this).find('.iconheader').text() + '.gif');
            // Advertiser-Specific - close anchor if visible
            if($('#nitro-anchor').length) {
                $('#nitro-anchor-close').click();
                $('#nitro-anchor').remove();
            }
        });
        // Preview model tester
        $('#zoomdisplay.modeltest').on('dragenter dragover', function(e) {
            e.preventDefault();
        }).on('dragleave drop', function(e) {
            e.preventDefault();
            if(e.originalEvent.dataTransfer.files.length) {
                $('body').css('overflow', 'hidden');
                $('#modeltest-text').remove();
                loadModel(e.originalEvent.dataTransfer.files[0], true);
                $('#zoom-controls').show();
            }
        });
        // Image viewer
        $('#zoomdisplay.imageview').on('dragenter dragover', function(e) {
            e.preventDefault();
        }).on('dragleave drop', function(e) {
            e.preventDefault();
            if(e.originalEvent.dataTransfer.files.length) {
                const file = e.originalEvent.dataTransfer.files[0];
                if(!file.type.startsWith('image/')) {
                    return;
                }
                $('body').css('overflow', 'hidden');
                $('#modeltest-text').remove();
                $('#zoom-grid').addClass('off');
                const reader = new FileReader();
                reader.onload = function(e) {
                    const img = $('<img>').attr('src', e.target.result);
                    img.on('load', function() {
                        $('#mag').click();
                        $('#zoom-controls').show();
                    });
                    $('#zoomdisplay').html('').append(img);
                    $('#zoomdisplay').append('<div id="pixel-grid" style="background-size: 1px 1px;"></div>');
                }
                reader.readAsDataURL(file);
            }
        });
        // Control select buttons with keyboard
        $(document).keydown(function(e) {
            const key = e.key.toLowerCase();
            if($('#zoomdisplay').hasClass('zoomed') && !$(document.activeElement).is('input')) {
                // Escape key to close viewer
                if(key == 'escape' && $('#zoom-reset').length && $('#zoom-reset').is(':visible')) {
                    $('#zoom-reset').click();
                    return false;
                }
                // Backspace to reset view
                if((key == 'backspace' || key == 'tab') && (($('#zoom-info').length && $('#zoom-info').is(':visible')) || ($('#zoom-cam').length && $('#zoom-cam').is(':visible')))) {
                    if($('#zoom-info').length && $('#zoom-info').is(':visible')) {
                        $('#zoom-info').click();
                    } else {
                        $('#zoom-cam').click();
                    }
                    return false;
                }
                // Up arrow to zoom in
                if(key == 'arrowup' && $('#zoom-in').length && $('#zoom-in').is(':visible')) {
                    $('#zoom-in').click();
                    return false;
                }
                // Down arrow to zoom out
                if(key == 'arrowdown' && $('#zoom-out').length && $('#zoom-out').is(':visible')) {
                    $('#zoom-out').click();
                    return false;
                }
                // Left arrow to move to previous zip image or GIF frame
                if(key == 'arrowleft' && (($('#zoom-previous').length && $('#zoom-previous').is(':visible')) || ($('#zoom-gifs-back').length && $('#zoom-gifs-back').is(':visible')))) {
                    if($('#zoom-previous').length && $('#zoom-previous').is(':visible')) {
                        $('#zoom-previous').click();
                    } else {
                        $('#zoom-gifs-back').click();
                    }
                    return false;
                }
                // Right arrow to move to next zip image or GIF frame
                if(key == 'arrowright' && (($('#zoom-next').length && $('#zoom-next').is(':visible')) || ($('#zoom-gifs-forward').length && $('#zoom-gifs-forward').is(':visible')))) {
                    if($('#zoom-next').length && $('#zoom-next').is(':visible')) {
                        $('#zoom-next').click();
                    } else {
                        $('#zoom-gifs-forward').click();
                    }
                    return false;
                }
                // R to rotate image or toggle turntable
                if(key == 'r' && (($('#zoom-rotate').length && $('#zoom-rotate').is(':visible')) || ($('#zoom-turn').length && $('#zoom-turn').is(':visible')))) {
                    if($('#zoom-rotate').length && $('#zoom-rotate').is(':visible')) {
                        $('#zoom-rotate').click();
                    } else {
                        $('#zoom-turn').click();
                    }
                    return false;
                }
                // F to flip image or toggle texture filtering
                if(key == 'f' && (($('#zoom-flip').length && $('#zoom-flip').is(':visible')) || ($('#zoom-tf').length && $('#zoom-tf').is(':visible')))) {
                    if($('#zoom-flip').length && $('#zoom-flip').is(':visible')) {
                        $('#zoom-flip').click();
                    } else {
                        $('#zoom-tf').click();
                    }
                    return false;
                }
                // G to toggle pixel grid or green background
                if(key == 'g' && (($('#zoom-grid').length && $('#zoom-grid').is(':visible')) || ($('#zoom-bg').length && $('#zoom-bg').is(':visible')))) {
                    if($('#zoom-grid').length && $('#zoom-grid').is(':visible')) {
                        $('#zoom-grid').trigger('mousedown');
                    } else {
                        $('#zoom-bg').trigger('mousedown');
                    }
                    return false;
                }
                // T to toggle wireframe
                if(key == 't' && $('#zoom-wire').length && $('#zoom-wire').is(':visible')) {
                    $('#zoom-wire').click();
                    return false;
                }
                // Space to play/pause GIF
                if(key == ' ' && $('#zoom-gifs-play').length && $('#zoom-gifs-play').is(':visible')) {
                    $('#zoom-gifs-play').click();
                    return false;
                }
                // P to toggle GIF ping pong
                if(key == 'p' && $('#zoom-gifs-pingpong').length && $('#zoom-gifs-pingpong').is(':visible')) {
                    $('#zoom-gifs-pingpong').click();
                    return false;
                }
                // Z to slow down GIF playback
                if(key == 'z' && $('#zoom-gifs-speed-down').length && $('#zoom-gifs-speed-down').is(':visible')) {
                    $('#zoom-gifs-speed-down').click();
                    return false;
                }
                // X to speed up GIF playback
                if(key == 'x' && $('#zoom-gifs-speed-up').length && $('#zoom-gifs-speed-up').is(':visible')) {
                    $('#zoom-gifs-speed-up').click();
                    return false;
                }
                // S to move image up
                if(key == 's' && $('#zoomdisplay > img').length) {
                    $('#zoomdisplay > img').css('top', Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) - ($('#zoomdisplay > img').width() >= $('#zoomdisplay > img').height() ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height()) / ((scale + 1) * 10 - (scale - 1) * 8)) + 'px');
                    $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                    return false;
                }
                // D to move image left
                if(key == 'd' && $('#zoomdisplay > img').length) {
                    $('#zoomdisplay > img').css('left', Math.round(parseInt($('#zoomdisplay > img').css('left'), 10) - ($('#zoomdisplay > img').width() >= $('#zoomdisplay > img').height() ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height()) / ((scale + 1) * 10 - (scale - 1) * 8)) + 'px');
                    $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
                    return false;
                }
                // W to move image down
                if(key == 'w' && $('#zoomdisplay > img').length) {
                    $('#zoomdisplay > img').css('top', Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) + ($('#zoomdisplay > img').width() >= $('#zoomdisplay > img').height() ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height()) / ((scale + 1) * 10 - (scale - 1) * 8)) + 'px');
                    $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                    return false;
                }
                // A to move image right
                if(key == 'a' && $('#zoomdisplay > img').length) {
                    $('#zoomdisplay > img').css('left', Math.round(parseInt($('#zoomdisplay > img').css('left'), 10) + ($('#zoomdisplay > img').width() >= $('#zoomdisplay > img').height() ? $('#zoomdisplay > img').width() : $('#zoomdisplay > img').height()) / ((scale + 1) * 10 - (scale - 1) * 8)) + 'px');
                    $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
                    return false;
                }
            }
        });
        // Zoom with mouse scroll wheel
        $('#zoomdisplay').on('wheel', function(e) {
            if($('#zoomdisplay > img').length) {
                e.preventDefault();
                const m1 = scale;
                let dir = 1;
                if(e.originalEvent.wheelDelta > 0 || e.originalEvent.detail < 0) {
                    $('#zoom-in').click();
                } else {
                    $('#zoom-out').click();
                    dir = -1;
                }
                if(m1 != scale && $(e.target).is('img')) {
                    $('#zoomdisplay > img').css('top', Math.round(parseInt($('#zoomdisplay > img').css('top'), 10) + ((($('#zoomdisplay').innerHeight() / 2) - (e.pageY - $('#zoomdisplay').offset().top)) / (scale + dir * -1) * dir)) + 'px');
                    $('#zoomdisplay > img').css('left', Math.round(parseInt($('#zoomdisplay > img').css('left'), 10) + ((($('#zoomdisplay').innerWidth() / 2) - (e.pageX - $('#zoomdisplay').offset().left)) / (scale + dir * -1) * dir)) + 'px');
                    $('#pixel-grid').css('top', gridPos($('#zoomdisplay > img').position().top) + 'px');
                    $('#pixel-grid').css('left', gridPos($('#zoomdisplay > img').position().left) + 'px');
                }
            }
        });
    }
    // Report form controls
    $('div[id^="report-button-"]').click(function() {
        const id = $(this).attr('id').split('_')[1];
        const type = $(this).attr('id').split('-')[2].split('_')[0];
        const target = $('#report-form-' + type + '_' + id);
        if(!target.is(':visible')) {
            target.slideDown('fast');
        } else {
            target.slideUp('fast');
        }
    });
    // Submit report
    $('.report-submit').click(function() {
        const container = $(this).parents('div[id^="report-form-"]');
        const type = container.children('input[name="type"]').val();
        const id = container.children('input[name="id"]').val();
        const report = container.children('textarea[name="report"]').val();
        $.ajax({
            url: '/process/report/',
            method: 'POST',
            data: {
                type: type,
                id: id,
                report: report,
                session_key: getSession()
            },
            success: function(response) {
                if(response) {
                    container.css('padding', '0px');
                    container.html(response);
                } else {
                    alert('Report is required and must be less than 5,000 characters to submit.');
                }
            },
            error: function(xhr, status, error) {
                alert('Error submitting report. Please try again.');
            }
        });
    });
    // Add asset conrols - categories
    $('#addasset-cat').on('change', function() {
        $(this).children('option[value=""]').remove();
        if($(this).val() == 'new') {
            $('#addasset-cat-new-row').fadeIn('fast');
            $('#addasset-cat-new').prop('disabled', false);
            $('#addasset-game-existing-row').hide();
            $('#addasset-game-new-label').text('Game:');
            $('tr[id^="addasset-game-new-"]').fadeIn('fast');
            $('tr[id^="addasset-game-new-"] input').prop('disabled', false);
            $('#addasset-section-existing-row').hide();
            $('#addasset-section-new-label').text('Section:');
            $('#addasset-section-new-row').fadeIn('fast');
            $('#addasset-section-new-row input').prop('disabled', false);
        } else {
            $('#addasset-cat-new-row').fadeOut('fast');
            $('#addasset-cat-new').prop('disabled', true);
            $('#addasset-game-existing-row').show();
            $('#addasset-game-new-label').text('');
            $('tr[id^="addasset-game-new-"]').hide();
            $('tr[id^="addasset-game-new-"] input').prop('disabled', true);
            $('#addasset-section-dd').text('(Select a Game)');
            $('#addasset-section-existing-row').show();
            $('#addasset-section-new-label').text('');
            $('#addasset-section-new-row').hide();
            $('#addasset-section-new-row input').prop('disabled', true);
            $('#addasset-game-dd').html('Loading...');
            $('#addasset-unofficial').prop('checked', $(this).val() == 3 || $('#addasset-unofficial').is(':checked'));
            $.ajax({
                url: '/process/gendd/',
                method: 'POST',
                data: {
                    type: 'game',
                    id: $('#addasset-cat').val(),
                    revision: $(this).data('revision') ? true : false,
                    status: $(this).data('status')
                },
                success: function(response) {
                    if(response) {
                        $('#addasset-game-dd').html(response);
                    } else {
                        alert('An error occurred retrieiving the game list. Please try again.');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error retrieving game list. Please try again.');
                }
            });
        }
    });
    // Add asset controls - games
    $('#addasset-game-dd').on('change', 'select', function() {
        $(this).children('option[value=""]').remove();
        if($(this).val() == 'new') {
            $('tr[id^="addasset-game-new-"]').fadeIn('fast');
            $('tr[id^="addasset-game-new-"] input').prop('disabled', false);
            $('#addasset-section-existing-row').hide();
            $('#addasset-section-new-label').text('Section:');
            $('#addasset-section-new-row').fadeIn('fast');
            $('#addasset-section-new-row input').prop('disabled', false);
        } else {
            $('tr[id^="addasset-game-new-"]').fadeOut('fast');
            $('tr[id^="addasset-game-new-"] input').prop('disabled', true);
            $('#addasset-section-existing-row').show();
            $('#addasset-section-new-label').text('');
            $('#addasset-section-new-row').hide();
            $('#addasset-section-new-row input').prop('disabled', true);
            $('#addasset-section-dd').html('Loading...');
            $.ajax({
                url: '/process/gendd/',
                method: 'POST',
                data: {
                    type: 'section',
                    id: $('#addasset-game').val()
                },
                success: function(response) {
                    if(response) {
                        $('#addasset-section-dd').html(response);
                    } else {
                        alert('An error occurred retrieiving the section list. Please try again.');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error retrieving section list. Please try again.');
                }
            });
        }
    });
    // Add asset controls - sections
    $('#addasset-section-dd').on('change', 'select', function() {
        $(this).children('option[value=""]').remove();
        if($(this).val() == 'new') {
            $('#addasset-section-new-row').fadeIn('fast');
            $('#addasset-section-new-row input').prop('disabled', false);
        } else {
            $('#addasset-section-new-row').hide();
            $('#addasset-section-new-row input').prop('disabled', true);
        }
    });
    // Trigger "new" fields when selecting new option
    $('#addasset select').each(function() {
        if($(this).val() == 'new') {
            $(this).trigger('change');
        }
    });
    // Auto-sort game sections
    $('.sect-order').on('blur', function() {
        $('#game-sections').append($('#game-sections tr').get().sort(function(a, b) {
            return($('.sect-order', a)).val() - $('.sect-order', b).val();
        }));
        let i = 0;
        const count = $('#game-sections tr').length;
        $('#game-sections tr').each(function() {
            i++;
            $(this).find('.sect-order').val(i).attr('tabindex', i);
            $(this).find('.sect-name').attr('tabindex', i + count);
        });
    });
    // Up/down game section arrows
    $('.sect-buttons div').click(function() {
        const target = $(this).closest('tr').find('.sect-order');
        const curr_order = parseInt(target.val(), 10);
        const change = $(this).data('dir') == 'up' ? -1 : 1;
        let new_order = curr_order + change + (0.5 * change);
        if(new_order < 1) {
            new_order = 0;
        }
        target.val(new_order);
        target.trigger('blur');
    });
    // Trigger up/down game section arrows with arrow keys
    $('.sect-order, .sect-name').on('keyup', function(e) {
        const key = e.key.toLowerCase();
        if(key == 'arrowup') {
            $(this).closest('tr').find('[data-dir="up"]').click();
            $(this).focus();
        } else if(key == 'arrowdown') {
            $(this).closest('tr').find('[data-dir="down"]').click();
            $(this).focus();
        }
    });
    // Auto-fill asset name from file Name
    $('#addasset #asset-file').change(function() {
        if($('#asset-name').val() == '') {
            const file = $(this).val();
            const index1 = file.lastIndexOf('\\');
            const index2 = file.lastIndexOf('.');
            const name = file.substring(index1 + 1, index2)
            $('#asset-name').val(name);
        }
    });
    // Check for reason on revisions
    $('form[action^="/process/revise"]').submit(function() {
        if($('#addasset_notes').length && $('#addasset_notes').val() == '') {
            alert('Reason is required.');
            $(this).find('input[type="submit"]').not('[name]').prop('disabled', false);
            return false;
        }
    });
    // Header search shortcuts
    $('form[id^="simple-searchform"]').on('submit', function() {
        const text = $(this).children('.header-search');
        const search = text.val();
        const prefix = search.substring(0, 2).toLowerCase();
        let action = '';
        if(prefix == 'a:' || prefix == 'g:') {
            text.val(search.substring(2).trim());
            action = prefix == 'a:' ? 'assets/' : 'games/';
        }
        $(this).attr('action', '/browse/' + action);
    });
    // Add or remove from favorites
    $('#favorite').click(function() {
        if($(this).hasClass('disabled')) {
            if($(this).data('reason') == 'logged-out') {
                alert('You must be logged in to save favorites.');
            } else if($(this).data('reason') == 'not-activated') {
                alert('You must activate your account before you can save favorites.');
            } else if($(this).data('reason') == 'staff-only') {
                alert('Favorites temporarily disabled - site in staff-only mode.');
            } else if($(this).data('reason') == 'view-as') {
                alert('You cannot save favorites in this view mode.');
            } else {
                alert('An unknown error has occurred - please contact staff if this error persists.');
            }
            return;
        }
        const type = $(this).data('type');
        const id = $(this).data('id');
        let action = null;
        if($(this).hasClass('favorited')) {
            $(this).removeClass('favorited');
            action = 'remove';
        } else {
            $(this).addClass('favorited');
            action = 'add';
        }
        $.ajax({
            url: '/process/favorite/',
            method: 'POST',
            data: {
                type: type,
                id: id,
                action: action,
                session_key: getSession()
            }
        });
    });
    // Twitch live button
    if($('.twitch-live').length) {
        setInterval(function() {
            const target = $('.twitch-live .material-symbols-outlined');
            const symbol = target.html();
            const new_symbol = symbol == 'radio_button_checked' ? 'radio_button_unchecked' : 'radio_button_checked';
            $('.twitch-live .material-symbols-outlined').html(new_symbol);
        }, 1000);
    }
    // Pending list filter controls - users
    $('#subfilter-user, #revfilter-user, #rejfilter-user').on('change', function() {
        const target = $(this).attr('id') == 'subfilter-user' ? 'sub' : ($(this).attr('id') == 'rejfilter-user' ? 'rej' : 'rev');
        $('#' + target + 'filter-cat').val('');
        $('#' + target + 'filter-game').html('  <option value="">- Game -</option>\n  <option value="">All</option>');
        $.ajax({
            url: '/process/gendd/',
            method: 'POST',
            data: {
                type: target + '-cat',
                id: $('#' + target + 'filter-user').val(),
                section: target == 'rev' ? $(this).data('section') : null
            },
            success: function(response) {
                if(response) {
                    $('#' + target + 'filter-cat').html(response);
                } else {
                    alert('An error occurred retrieiving the console list. Please try again.');
                }
            },
            error: function(xhr, status, error) {
                alert('Error retrieving console list. Please try again.');
            }
        });
    });
    // Pending list filter controls - categories
    $('#subfilter-cat, #revfilter-cat, #rejfilter-cat').on('change', function() {
        const target = $(this).attr('id') == 'subfilter-cat' ? 'sub' : ($(this).attr('id') == 'rejfilter-cat' ? 'rej' : 'rev');
        if($(this).val() != '') {
            $('#' + target + 'filter-game').val('');
            $.ajax({
                url: '/process/gendd/',
                method: 'POST',
                data: {
                    type: target + '-game',
                    id: $('#' + target + 'filter-cat').val(),
                    uid: $('#' + target + 'filter-user').val(),
                    section: target == 'rev' ? $(this).data('section') : null
                },
                success: function(response) {
                    if(response) {
                        $('#' + target + 'filter-game').html(response);
                    } else {
                        alert('An error occurred retrieiving the game list. Please try again.');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error retrieving game list. Please try again.');
                }
            });
        } else {
            $('#' + target + 'filter-game').html('  <option value="">- Game -</option>\n  <option value="">All</option>');
        }
    });
    // Pending list approve and reject dropdowns
    $('.pending-button.approve:not(.disabled), .pending-button.reject:not(a)').click(function() {
        const target_class = $(this).hasClass('approve') ? '.pending-approve' : '.pending-reject';
        const hide = target_class == '.pending-approve' ? '.pending-reject' : '.pending-approve';
        const target = $(this).parents().children(target_class);
        $(this).parents().children(hide).hide();
        if(target.is(':visible')) {
            target.fadeOut('fast');
        } else {
            target.fadeIn('fast');
        }
    });
    // Approve button handling
    $('.approve-confirm').click(function() {
        const id = $(this).data('id');
        const type = $(this).data('type');
        const options = type.startsWith('revision-') ? $('#pending-' + id).find('input[name="approve[]"]:checked').map(function() {
            return $(this).val();
        }).get() : null;
        const update = type == 'revision-asset' ? $('#pending-' + id).find('input[name="update"]:checked').length : null;
        const preserve = type == 'revision-game' ? $('#pending-' + id).find('input[name="preserve"]:checked').length : null;
        const notes = $('#pending-' + id).find('input[name="notes"]').val();
        if(type.startsWith('revision-') && !options.length) {
            alert('You must select at least one option to approve.');
            return;
        }
        if(id) {
            $.ajax({
                url: type == 'submission' ? '/process/approveasset/' : (type == 'revision-asset' ? '/process/approverev/' : '/process/approvegrev/'),
                method: 'POST',
                data: {
                    id: id,
                    options: options,
                    update: update,
                    preserve: preserve,
                    notes: notes,
                    session_key: getSession()
                },
                success: function(response) {
                    if(response) {
                        $('#pending-' + id).slideUp('fast', function() {
                            $('#pending-' + id).remove();
                            if(!$('.pending').length) {
                                window.location.reload();
                            }
                        });
                    } else {
                        alert('An unknown error occurred. Please refresh and try again.');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error approving submission. Please refresh and try again.');
                }
            });
        }
    });
    // Reject button handling
    $('.reject-confirm').click(function() {
        const id = $(this).data('id');
        const type = $(this).data('type');
        const options = type == 'submission' ? $('#pending-' + id).find('input[name="reason[]"]:checked').map(function() {
            return $(this).val();
        }).get() : null;
        const other = type == 'submission' ? $('#pending-' + id).find('input[name="other"]:checked').length : null;
        const other_text = type != 'submission' || other ? $('#pending-' + id).find('input[name="other_text"]').val() : null;
        const del = type =='submission' ? $('#pending-' + id).find('input[name="delete"]:checked').length : null;
        if(type == 'submission' && !options.length && !other) {
            alert('You must select at least one reason to reject.');
            return;
        }
        if((type != 'submission' || other) && other_text == '') {
            alert(type == 'submission' ? 'You must specify a reason when "Other" is selected.' : 'You must supply a reason.');
            return;
        }
        if(id) {
            $.ajax({
                url: type == 'submission' ? '/process/rejectasset/' : (type == 'revision-asset' ? '/process/rejectrev/' : '/process/rejectgrev/'),
                method: 'POST',
                data: {
                    id: id,
                    options: options,
                    other: other,
                    other_text: other_text,
                    delete: del,
                    session_key: getSession()
                },
                success: function(response) {
                    if(response) {
                        $('#pending-' + id).slideUp('fast', function() {
                            $('#pending-' + id).remove();
                            if(!$('.pending').length) {
                                window.location.reload();
                            }
                        });
                    } else {
                        alert('An unknown error occurred. Please refresh and try again.');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error rejecting submission. Please refresh and try again.');
                }
            });
        }
    });
    // Batch reject handling
    $('#batch-reject').click(function(e) {
        e.preventDefault();
        const type = $(this).data('type');
        const reason = $('#batch-reason').val();
        const filter = $('#batch-filter').val();
        const spam = type == 'submission' && $('#batch-spam').is(':checked') ? true : false;
        if(reason == '') {
            alert('You must supply a reason when batch rejecting.');
            return;
        }
        if(!confirm('Are you sure? Click OK to confirm.')) {
            return;
        }
        if(spam && !confirm('You have marked all filtered items as spam. They will be deleted and the user(s) will receive permanent marks on their accounts if you proceed. Are you sure? Click OK again to confirm.')) {
            return;
        }
        $.ajax({
            url: type == 'submission' ? '/process/batchasset/' : (type == 'revision-asset' ? '/process/batchrev/' : '/process/batchgrev/'),
            method: 'POST',
            data: {
                reason: reason,
                filter: filter,
                spam: spam,
                session_key: getSession()
            },
            success: function(response) {
                if(response) {
                    window.location.href = (type == 'submission' ? '/pending/' : (type == 'revision-asset' ? '/revisions/assets/' : '/revisions/games/')) + '?message=batch-success';
                } else {
                    alert('An unknown error occurred. Please refresh and try again.');
                }
            },
            error: function(xhr, status, error) {
                alert('Error processing batch rejection. Please refresh and try again.');
            }
        });
    });
    // Game info floaty
    $('.pending .new-game').on('mouseover', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_game-info_' + id).fadeIn('fast');
    }).on('mouseout', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_game-info_' + id).fadeOut('fast');
    });
    // Submission notes floaty
    $('.pending .pending-notes').on('mouseover', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_notes_' + id).fadeIn('fast');
    }).on('mouseout', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_notes_' + id).fadeOut('fast');
    });
    // Asset / game icon floaty
    $('.pending .highlight .iconcontainer img').on('mouseover', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_icon_' + id).fadeIn('fast');
    }).on('mouseout', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_icon_' + id).fadeOut('fast');
    });
    // Preview icon floaty
    $('.pending .icon-meta.meta-right, .pending .preview-icon-rev span').on('mouseover', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_preview-icon_' + id).fadeIn('fast');
    }).on('mouseout', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_preview-icon_' + id).fadeOut('fast');
    });
    // Zip issues floaty
    $('.pending .pending-issues span').on('mouseover', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_pending-issues_' + id).fadeIn('fast');
    }).on('mouseout', function() {
        const id = $(this).parents('.pending').attr('id').split('-')[1];
        $('#floaty_pending-issues_' + id).fadeOut('fast');
    });
    // Alternative game name controls
    $('button#new-altname').click(function() {
        let index = 0;
        if($('#altnames').find('div.newname').length) {
            index = parseInt($('#altnames').find('div.newname:last input').attr('name').match(/newnames\[([0-9]+)\]/)[1], 10) + 1;
        }
        $('#altnames').append('<div class="newname">\n<input type="text" name="newnames[' + index + '][name]" placeholder="New Name" class="formElement" style="width: calc(100% - 170px);">\n<select name="newnames[' + index + '][type]" class="formElement" style="max-width: 100px;">' + $('#altname-template').html() + '</select>\n<span class="material-symbols-outlined">disabled_by_default</span>\n</div>\n');
        $('#altnames input:last').focus();
    });
    $('#altnames').on('click', 'span', function() {
        $(this).parent().remove();
    });
    // Animated gif submission controls - add
    $('button#new-gif').click(function() {
        let index = 0;
        if($('.gifs').find('div.newgif').length) {
            index = parseInt($('.gifs').find('div.newgif:last input').attr('name').match(/newgifs\[([0-9]+)\]/)[1], 10) + 1;
        }
        $('.gifs').append('<div class="newgif">\n<input type="text" name="newgifs[' + index + ']" placeholder="Name" class="formElement" style="width: calc(100% - 210px);">\n<input type=\"file\" name=\"newgifs[' + index + ']\">\n<span class="material-symbols-outlined">disabled_by_default</span>\n</div>\n');
        $('.gifs input:last').focus();
    });
    // Animated gif submission controls - remove
    $('.form .gifs').on('click', 'span.material-symbols-outlined', function() {
        $(this).parent().remove();
    });
    // Animated gif submission controls - preview
    $('.gifs .gifcontainer').on('mouseover', 'img', function() {
        $(this).parents().eq(1).find('.floaty').fadeIn('fast');
    }).on('mouseout', function() {
        $(this).parents().eq(1).find('.floaty').fadeOut('fast');
    });
    // Reject reasons toggle
    $('.reject-toggle').click(function() {
        const target = $(this).parents().children('.reject-reasons');
        if(target.is(':visible')) {
            target.fadeOut('fast');
        } else {
            target.fadeIn('fast');
        }
    });
    // Announcement countdowns
    if($('#announcement').length && $('#announcement').data('countdown') && $('#countdown').length) {
        let now, t, r, d, h, m, s;
        let target = parseInt($('#announcement').data('countdown'), 10);
        function updateCountdown() {
            t = Math.floor(now / 1000);
            r = target - t;
            if(target > t) {
                d = Math.floor(r / 86400);
                h = Math.floor(r % 86400 / 3600);
                m = Math.floor(r % 3600 / 60);
                s = Math.floor(r % 60);
            } else {
                $('#countdown').html(0 + '<span class="separator" title="seconds">s</span>');
                return false;
            }
            d = (d > 0) ? (d + '<span class="separator" title="days">d</span>') : '';
            h = (h > 0 || d) ? ((d && h < 10 ? '<span class="padding">0</span>' : '') + h + '<span class="separator" title="hours">h</span>') : '';
            m = (m > 0 || h) ? ((h && m < 10 ? '<span class="padding">0</span>' : '') + m + '<span class="separator" title="minuites">m</span>') : '';
            s = (m && s < 10 ? '<span class="padding">0</span>' : '') + s + '<span class="separator" title="seconds">s</span>';
            $('#countdown').html(d + h + m + s);
            return true;
        }
        function timer(callback) {
            let frameFunc = function() {
                now = 1000 * Math.floor(Date.now() / 1000 + 0.1);
                if(callback()) {
                    setTimeout(timerFunc, now + 1000 - Date.now());
                }
            }, timerFunc = function() {
                requestAnimationFrame(frameFunc);
            };
            timerFunc();
        }
        timer(updateCountdown);
    }
    // Update displayed time on profile editor
    $('#timezone').change(function() {
        if($('#time-display').length) {
            $('#time-display').html('(' + (new Intl.DateTimeFormat('en-US', { timeZone: $(this).val(), hour: 'numeric', minute: 'numeric' }).format(new Date())) + ')');
        }
    });
    // Drag and drop file processing
    $('.iconcontainer').on('dragenter dragover', function(e) {
        e.preventDefault();
    }).on('dragleave drop', function(e) {
        e.preventDefault();
        if(!$('#staff-menu').length) {
            return;
        }
        if(['addasset', 'deleted', 'revisions'].some(nodrop => page.includes(nodrop)) || $(this).hasClass('gif')) {
            return;
        }
        let type = $(this).hasClass('game') ? 'game' : ($(this).hasClass('preview') ? 'preview' : 'asset');
        if(window.location.hostname.includes('sounds') && type != 'game') {
            return;
        }
        if(e.originalEvent.dataTransfer.files.length) {
            let id = 0;
            if($(this).closest('.pending').length) {
                id = parseInt($(this).closest('.pending').attr('id').split('-')[1], 10);
            } else {
                let src = $(this).find('img');
                if(src.length) {
                    src = src.attr('src').split('/');
                    if(src.length == 5) {
                        id = parseInt(src[4].split('.')[0], 10);
                    }
                }
            }
            const body = $(this).find('.iconbody');
            const file = e.originalEvent.dataTransfer.files[0];
            const ext = file.name.split('.').at(-1).toLowerCase();
            const current_ext = page.includes('/pending/') ? $(this).parents('.pending').find('[data-ext]').data('ext') : null;
            const url = ext == 'glb' ? 'editpreview' : (ext == 'zip' ? 'editzip' : 'editicon');
            if(!['png', 'gif', 'zip', 'glb'].includes(ext)) {
                return;
            }
            if(ext == 'glb' && (!window.location.hostname.includes('models') || type != 'asset')) {
                return;
            }
            if(ext == 'zip' && (!page.includes('/pending/') || type != 'asset' || current_ext != 'zip' || !confirm('Are you sure you want to replace this asset? Click OK to confirm.'))) {
                return;
            }
            if(ext == 'png' && window.location.hostname.includes('models') && page.includes('/pending/') && type == 'asset' && file.name.includes('_preview')) {
                type = 'preview';
            }
            let formData = new FormData();
            formData.append('type', type);
            formData.append('id', id);
            formData.append('file', file);
            formData.append('session_key', getSession());
            $.ajax({
                url: '/process/' + url + '/',
                method: 'POST',
                data: formData,
                processData: false,
                contentType: false,
                success: function(response) {
                    if(!response) {
                        alert('An error occurred. Please try again.');
                        return;
                    }
                    if(url == 'editicon') {
                        if(type == 'asset' || !page.includes('/pending/')) {
                            let reader = new FileReader();
                            reader.onload = function(e) {
                                let img = $('<img>').attr('src', e.target.result);
                                body.html('').append(img);
                            }
                            reader.readAsDataURL(file);
                        } else if(type == 'preview' && page.includes('/pending/')) {
                            body.parent().find('.icon-meta.meta-right').remove();
                            body.parent().append('<div class="icon-meta meta-right material-symbols-outlined thin-stroke" style="color: var(--bg-success);">photo_size_select_large</div>');
                        } else {
                            alert(response);
                        }
                    } else if(url == 'editpreview') {
                        body.parent().append('<div class="icon-zip material-symbols-outlined thin-stroke" title="3D Preview Available" style="color: var(--bg-success);">view_in_ar</div>');
                    } else {
                        body.parents('.pending').find('.pending-upload').css('background-color', 'var(--bg-success)');
                    }
                },
                error: function(xhr, status, error) {
                    if(url == 'editicon') {
                        body.html('<span class="material-symbols-outlined" style="color: var(--text-error); font-variation-settings: \'FILL\' 0;">hide_image</span>');
                    }
                    alert('Error processing dropped file. Please refresh and try again.');
                }
            });
        }
    });
    // Staff calendar
    const headers = { 0: 'Global', 1: 'Sprites', 2: 'Models', 3: 'Textures', 4: 'Sounds' };
    const labels = { submit: 'Submissions', approve: 'Approved', reject: 'Rejected', batch: 'Batch Rejected', net: 'Net', users: 'New Users' };
    const items = { submit: '2_Submit', approve: '1_Approve', reject: '2_Reject', batch: '2_Batch+Reject' };
    $('#staff-calendar[data-month]').on('click', '.calendar-day[data-day]:not(.selected)', function() {
        const date = $('#staff-calendar').data('month') + '-' + $(this).data('day').toString().padStart(2, '0');
        const details = $.parseJSON($(this).find('input[name="details"]').val());
        const staff = $(this).find('input[name="staff"]').val();
        $('#staff-calendar .calendar-day').removeClass('selected');
        $(this).addClass('selected');
        $('#staff-calendar-details').html('');
        $('#staff-calendar-details').append('<div style="font-weight: bold;">Activity for ' + date + ':</div>');
        $.each(details, function(i, data) {
            let target = $('#staff-calendar-details');
            if(i > 0) {
                target = $('<div class="calendar-details-site"></div>');
                $('#staff-calendar-details').append(target);
            }
            let color = data.net <= 0 ? 'green' : 'red';
            target.append('<div class="calendar-details-header ' + color + '">' + headers[i] + ':</div>\n');
            $.each(data, function(j, val) {
                let sign = j == 'net' && val > 0 ? '+' : '';
                if(Object.keys(items).includes(j)) {
                    let filter_site = i > 0 ? ('site=' + i + '&') : '';
                    target.append('<a href="/log/?' + filter_site + 'item=' + items[j] + '&date=' + date + '" class="pending-tag"><span class="browse-tag-catname">' + labels[j] + '</span> ' + sign + val.toLocaleString() + '</a>\n');
                } else {
                    target.append('<span class="pending-tag"><span class="browse-tag-catname">' + labels[j] + '</span> ' + sign + val.toLocaleString() + '</span>\n');
                }
            });
        });
        if(staff.length) {
            $('#staff-calendar-details').append('<div id="active-staff" class="calendar-details-header">Active Staff:</div>\n');
            $('#staff-calendar-details').append('<div id="active-staff-list"><span class="pending-tag">Loading active staff list...</span></div>\n');
            $.ajax({
                url: '/process/calendar/',
                method: 'POST',
                data: {
                    uids: staff
                },
                success: function(response) {
                    if(response) {
                        const json = $.parseJSON(response);
                        let staff_list = '';
                        $('#active-staff').html('Active Staff (' + json.count.toLocaleString() + '):\n');
                        $.each(json.staff, function(i, name) {
                            staff_list += '<span class="pending-tag">' + name + '</span>\n';
                        });
                        $('#active-staff-list').html(staff_list);
                    } else {
                        $('#active-staff-list').html('<span class="pending-tag">Failed to load staff list. Please try again.</span>');
                    }
                },
                error: function(xhr, status, error) {
                    alert('Error loading data. Please try again.');
                }
            });
        }
        $('#staff-calendar-details').slideDown('fast');
    });
    // Share button
    $('#share').click(async function() {
        $(this).blur();
        const url = window.location.origin + page;
        const title = document.title;
        if(navigator.share) {
            try {
                await navigator.share({
                    title: title,
                    url: url
                });
                return;
            } catch(e) {
                console.log('Share failed: ', e);
            }
            return;
        }
        try {
            await navigator.clipboard.writeText(url);
            alert('Link copied to clipboard!');
        } catch(e) {
            console.error('Copy to clipboard failed: ', e);
            prompt('Copy this link: ', url);
        }
    });
    // Drag and drop files on add/edit forms
    $('#addasset').on('dragenter dragover', function(e) {
        if(!$(e.target).is('input[type="file"]')) {
            e.preventDefault();
        }
    }).on('dragleave drop', function(e) {
        if($(e.target).is('input[type="file"]') || !e.originalEvent.dataTransfer.files.length) {
            return;
        }
        e.preventDefault();
        for(const [key, file] of Object.entries(e.originalEvent.dataTransfer.files)) {
            let ext = file.name.split('.').at(-1).toLowerCase();
            let dataTransfer = new DataTransfer();
            dataTransfer.items.add(file);
            if(ext == 'zip' || file.name.includes('_asset')) {
                document.getElementById('asset-file').files = dataTransfer.files;
            } else if(ext == 'glb') {
                document.getElementById('preview-model-file').files = dataTransfer.files;
            } else if(file.name == 'icon.png' || file.name.includes('_icon')) {
                document.getElementById('asset-icon-file').files = dataTransfer.files;
            } else if(file.name == 'preview.png' || file.name.includes('_preview')) {
                document.getElementById('preview-icon-file').files = dataTransfer.files;
            } else if(file.name == 'gameicon.png' || file.name.includes('_gameicon')) {
                document.getElementById('game-icon-file').files = dataTransfer.files;
            }
        }
    });
});
