use std::cell::RefCell;
use std::rc::Rc;

use wxdragon::prelude::*;

use crate::core::{self, Language};

struct State {
    langs: Vec<Language>,
    checked: Vec<bool>,
    /// Indexes into `langs` of the rows currently shown (after the search filter).
    visible: Vec<usize>,
}

#[derive(Clone)]
struct Ui {
    frame: Frame,
    status: StaticText,
    search: TextCtrl,
    list: CheckListBox,
    selected: StaticText,
    install_btn: Button,
    uninstall_btn: Button,
    log: TextCtrl,
}

pub fn run(start_uninstall: bool) {
    wxdragon::main(move |_app| {
        let frame = Frame::builder()
            .with_title(&format!("Clipboard Voice {} Installer", core::VERSION))
            .with_size(Size::new(720, 600))
            .build();
        let panel = Panel::builder(&frame).build();
        let sizer = BoxSizer::builder(Orientation::Vertical).build();

        let status = StaticText::builder(&panel).with_label("").build();
        let intro = StaticText::builder(&panel)
            .with_label(
                "Clipboard Voice is a SAPI voice that puts the text programs speak on the clipboard. \
                 It adds one voice per language you check below.",
            )
            .build();

        // In wxWidgets on Windows, the label right before a control is the name a screen reader gives it.
        let search_label = StaticText::builder(&panel).with_label("Search languages (name or code, e.g. spanish or es-MX):").build();
        let search = TextCtrl::builder(&panel).build();
        let list_label = StaticText::builder(&panel).with_label("Languages to install (space checks or unchecks):").build();
        let list = CheckListBox::builder(&panel).build();
        let selected = StaticText::builder(&panel).with_label("").build();

        let btn_sizer = BoxSizer::builder(Orientation::Horizontal).build();
        let install_btn = Button::builder(&panel).with_label("Install").build();
        let uninstall_btn = Button::builder(&panel).with_label("Uninstall").build();
        let close_btn = Button::builder(&panel).with_label("Close").build();
        btn_sizer.add_stretch_spacer(1);
        btn_sizer.add(&install_btn, 0, SizerFlag::All, 4);
        btn_sizer.add(&uninstall_btn, 0, SizerFlag::All, 4);
        btn_sizer.add(&close_btn, 0, SizerFlag::All, 4);

        let log_label = StaticText::builder(&panel).with_label("Log:").build();
        let log = TextCtrl::builder(&panel)
            .with_style(TextCtrlStyle::MultiLine | TextCtrlStyle::ReadOnly | TextCtrlStyle::WordWrap)
            .build();

        sizer.add(&status, 0, SizerFlag::Expand | SizerFlag::All, 8);
        sizer.add(&intro, 0, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right, 8);
        sizer.add(&search_label, 0, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right | SizerFlag::Top, 8);
        sizer.add(&search, 0, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right, 8);
        sizer.add(&list_label, 0, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right | SizerFlag::Top, 8);
        sizer.add(&list, 2, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right, 8);
        sizer.add(&selected, 0, SizerFlag::Expand | SizerFlag::All, 8);
        sizer.add_sizer(&btn_sizer, 0, SizerFlag::Expand | SizerFlag::All, 4);
        sizer.add(&log_label, 0, SizerFlag::Expand | SizerFlag::Left | SizerFlag::Right, 8);
        sizer.add(&log, 1, SizerFlag::Expand | SizerFlag::All, 8);
        panel.set_sizer(sizer, true);

        let ui = Ui { frame: frame.clone(), status, search: search.clone(), list: list.clone(), selected, install_btn: install_btn.clone(), uninstall_btn: uninstall_btn.clone(), log };

        let langs = core::all_languages();
        let installed: Vec<String> = core::installed_voices().into_iter().map(|v| v.code).collect();
        let mut checked: Vec<bool> = langs.iter().map(|l| installed.iter().any(|c| c.eq_ignore_ascii_case(&l.code))).collect();
        if installed.is_empty() {
            if let Some(i) = langs.iter().position(|l| l.code == "en") {
                checked[i] = true;
            }
        }
        let state = Rc::new(RefCell::new(State { langs, checked, visible: Vec::new() }));
        fill_list(&ui, &state);
        refresh(&ui, &state);

        {
            let (ui_c, state_c) = (ui.clone(), state.clone());
            search.on_text_updated(move |_| fill_list(&ui_c, &state_c));
        }
        {
            let (ui_c, state_c) = (ui.clone(), state.clone());
            list.on_toggled(move |e| {
                let Some(row) = e.get_selection() else { return };
                {
                    let mut s = state_c.borrow_mut();
                    let Some(&idx) = s.visible.get(row as usize) else { return };
                    s.checked[idx] = ui_c.list.is_checked(row);
                }
                update_selected(&ui_c, &state_c);
            });
        }
        {
            let (ui_c, state_c) = (ui.clone(), state.clone());
            install_btn.on_click(move |_| do_install(&ui_c, &state_c));
        }
        {
            let (ui_c, state_c) = (ui.clone(), state.clone());
            uninstall_btn.on_click(move |_| do_uninstall(&ui_c, &state_c));
        }
        {
            let frame_c = frame.clone();
            close_btn.on_click(move |_| {
                frame_c.close(true);
            });
        }

        frame.show(true);
        if start_uninstall {
            // Opened from Settings > Apps > Uninstall.
            do_uninstall(&ui, &state);
        } else {
            search.set_focus();
        }
    })
    .expect("Failed to start application");
}

fn checked_codes(state: &State) -> Vec<(String, String)> {
    state.langs.iter().zip(&state.checked).filter(|(_, &c)| c).map(|(l, _)| (l.code.clone(), l.english.clone())).collect()
}

/// Rebuild the list rows for the current search text, keeping check marks.
fn fill_list(ui: &Ui, state: &Rc<RefCell<State>>) {
    let query = ui.search.get_value().trim().to_lowercase();
    let mut s = state.borrow_mut();
    s.visible = (0..s.langs.len())
        .filter(|&i| query.is_empty() || s.langs[i].label.to_lowercase().contains(&query))
        .collect();
    ui.list.clear();
    for (row, &idx) in s.visible.iter().enumerate() {
        ui.list.append(&s.langs[idx].label);
        if s.checked[idx] {
            ui.list.check(row as u32, true);
        }
    }
    if !s.visible.is_empty() {
        ui.list.set_selection(0, true);
    }
    drop(s);
    update_selected(ui, state);
}

fn update_selected(ui: &Ui, state: &Rc<RefCell<State>>) {
    let s = state.borrow();
    let chosen = checked_codes(&s);
    let shown = if s.visible.len() == s.langs.len() {
        format!("{} languages listed.", s.langs.len())
    } else {
        format!("{} of {} languages match the search.", s.visible.len(), s.langs.len())
    };
    let text = if chosen.is_empty() {
        format!("No language checked. {shown}")
    } else {
        let names: Vec<String> = chosen.iter().map(|(c, n)| format!("{n} ({c})")).collect();
        format!("Checked: {}. {shown}", names.join(", "))
    };
    ui.selected.set_label(&text);
    ui.install_btn.enable(!chosen.is_empty());
}

fn refresh(ui: &Ui, state: &Rc<RefCell<State>>) {
    let voices = core::installed_voices();
    if voices.is_empty() {
        ui.status.set_label("Clipboard Voice is not installed.");
        ui.install_btn.set_label("Install");
    } else {
        let names: Vec<String> = voices.iter().map(|v| v.name.clone()).collect();
        ui.status.set_label(&format!("Installed voices: {}.", names.join(", ")));
        ui.install_btn.set_label("Apply languages");
    }
    ui.uninstall_btn.enable(core::is_installed());
    update_selected(ui, state);
}

fn do_install(ui: &Ui, state: &Rc<RefCell<State>>) {
    let codes: Vec<String> = checked_codes(&state.borrow()).into_iter().map(|(c, _)| c).collect();
    if codes.is_empty() {
        show_message(&ui.frame, "Check at least one language first.", "Nothing to install", true);
        return;
    }
    set_busy(ui, true);
    ui.status.set_label("Installing...");
    let log = ui.log.clone();
    let result = core::install(&codes, &mut |m| log_append(&log, m));
    set_busy(ui, false);
    refresh(ui, state);
    match result {
        Ok(voices) => {
            let names: Vec<String> = voices.iter().map(|v| v.name.clone()).collect();
            show_message(
                &ui.frame,
                &format!(
                    "Clipboard Voice is installed. Programs that use SAPI voices now list:\n\n{}\n\nPrograms that were already open may need a restart to see new voices.",
                    names.join("\n")
                ),
                "Installation complete",
                false,
            );
        }
        Err(e) => {
            log_append(&ui.log, &format!("Error: {e}"));
            show_message(&ui.frame, &format!("Installation failed: {e}"), "Installation failed", true);
        }
    }
}

fn do_uninstall(ui: &Ui, state: &Rc<RefCell<State>>) {
    let confirm = MessageDialog::builder(&ui.frame, "Remove Clipboard Voice and all of its voices?", "Uninstall Clipboard Voice")
        .with_style(MessageDialogStyle::YesNo | MessageDialogStyle::IconQuestion)
        .build();
    if confirm.show_modal() != ID_YES {
        return;
    }
    set_busy(ui, true);
    let log = ui.log.clone();
    let result = core::uninstall(&mut |m| log_append(&log, m));
    set_busy(ui, false);
    refresh(ui, state);
    match result {
        Ok(()) => show_message(&ui.frame, "Clipboard Voice has been uninstalled.", "Uninstall complete", false),
        Err(e) => show_message(&ui.frame, &format!("Uninstall finished with problems: {e}"), "Uninstall", true),
    }
}

fn set_busy(ui: &Ui, busy: bool) {
    ui.install_btn.enable(!busy);
    ui.uninstall_btn.enable(!busy);
    ui.list.enable(!busy);
    ui.search.enable(!busy);
}

fn show_message(parent: &Frame, msg: &str, title: &str, error: bool) {
    let icon = if error { MessageDialogStyle::IconError } else { MessageDialogStyle::IconInformation };
    MessageDialog::builder(parent, msg, title).with_style(MessageDialogStyle::OK | icon).build().show_modal();
}

fn log_append(log: &TextCtrl, msg: &str) {
    let current = log.get_value();
    if current.is_empty() {
        log.set_value(msg);
    } else {
        log.set_value(&format!("{current}\n{msg}"));
    }
}
