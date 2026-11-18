#![windows_subsystem = "windows"]

//! Clipboard Voice installer.
//!
//!   ClipboardVoiceInstaller.exe                        window (default)
//!   ClipboardVoiceInstaller.exe --uninstall            window, asks to uninstall (Settings > Apps)
//!   ClipboardVoiceInstaller.exe --cli                  interactive console menu
//!   ClipboardVoiceInstaller.exe --cli --install en,es  install exactly these languages
//!   ... --install en,es --output prism                 which voices: clipboard, prism or both
//!                                                      (default: the ones installed now, else clipboard)
//!   ClipboardVoiceInstaller.exe --cli --uninstall      remove everything
//!   ClipboardVoiceInstaller.exe --cli --list           list every language code you can use
//!   ... --log <file>                                   also write console output to a file

mod cli;
mod core;
mod gui;

use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let has = |flag: &str| args.iter().any(|a| a.eq_ignore_ascii_case(flag));
    let value_of = |flag: &str| {
        args.iter().position(|a| a.eq_ignore_ascii_case(flag)).and_then(|i| args.get(i + 1)).cloned()
    };

    if !has("--cli") {
        gui::run(has("--uninstall"));
        return;
    }

    let output = match value_of("--output") {
        None => None,
        Some(v) => match core::Output::parse(&v) {
            Some(o) => Some(o),
            None => {
                attach_console();
                println!("Unknown --output '{v}': use clipboard, prism or both.");
                std::process::exit(2);
            }
        },
    };
    let action = if let Some(langs) = value_of("--install") {
        cli::Action::Install(
            langs.split([',', ';']).map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect(),
            output,
        )
    } else if has("--uninstall") {
        cli::Action::Uninstall
    } else if has("--list") {
        cli::Action::List
    } else {
        cli::Action::Menu
    };
    let interactive = matches!(action, cli::Action::Menu);

    let own_console = attach_console();
    let mut log_file = value_of("--log").and_then(|p| std::fs::File::create(p).ok());
    let mut out = |line: &str| {
        println!("{line}");
        if let Some(f) = log_file.as_mut() {
            let _ = writeln!(f, "{line}");
        }
    };
    let code = cli::run(action, &mut out);
    if own_console && !interactive && !has("--log") {
        // The console window was opened for us (e.g. after a UAC prompt); keep it readable.
        print!("Press Enter to close.");
        let _ = std::io::stdout().flush();
        let _ = std::io::stdin().read_line(&mut String::new());
    }
    std::process::exit(code);
}

/// Uses the console we were started from, or opens one (an elevated process
/// started from a normal console gets a fresh console from UAC). Returns true
/// when the console is our own.
fn attach_console() -> bool {
    use windows_sys::Win32::System::Console::{AllocConsole, AttachConsole, ATTACH_PARENT_PROCESS};
    unsafe {
        if AttachConsole(ATTACH_PARENT_PROCESS) != 0 {
            false
        } else {
            AllocConsole();
            true
        }
    }
}
