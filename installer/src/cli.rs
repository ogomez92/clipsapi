//! Console mode: `--cli` alone is an interactive menu; with `--install`,
//! `--uninstall` or `--list` it runs that one action and exits (for scripts).

use std::io::{self, BufRead, Write};

use crate::core;

pub enum Action {
    Menu,
    Install(Vec<String>),
    Uninstall,
    List,
}

/// Returns the process exit code.
pub fn run(action: Action, out: &mut dyn FnMut(&str)) -> i32 {
    out(&format!("=== Clipboard Voice installer {} ===", core::VERSION));
    match action {
        Action::Install(codes) => report(core::install(&codes, out).map(|_| ()), out),
        Action::Uninstall => report(core::uninstall(out), out),
        Action::List => {
            for l in core::all_languages() {
                out(&format!("{}\t{}", l.code, l.label));
            }
            0
        }
        Action::Menu => menu(out),
    }
}

fn report(result: Result<(), String>, out: &mut dyn FnMut(&str)) -> i32 {
    match result {
        Ok(()) => {
            out("Done.");
            0
        }
        Err(e) => {
            out(&format!("Error: {e}"));
            1
        }
    }
}

fn show_status(out: &mut dyn FnMut(&str)) {
    let voices = core::installed_voices();
    if voices.is_empty() {
        out("Clipboard Voice is not installed.");
    } else {
        out("Installed voices:");
        for v in voices {
            out(&format!("  {} ({})", v.name, v.code));
        }
    }
}

fn prompt(text: &str) -> Option<String> {
    print!("{text}");
    let _ = io::stdout().flush();
    let mut line = String::new();
    match io::stdin().lock().read_line(&mut line) {
        Ok(0) | Err(_) => None,
        Ok(_) => Some(line.trim().to_string()),
    }
}

fn menu(out: &mut dyn FnMut(&str)) -> i32 {
    loop {
        out("");
        show_status(out);
        out("");
        out("  1. Install, or change languages");
        out("  2. Search the language list");
        out("  3. Uninstall");
        out("  4. Exit");
        let Some(choice) = prompt("Choose an option (1-4): ") else { return 0 };
        out("");
        match choice.as_str() {
            "1" => {
                let current: Vec<String> = core::installed_voices().into_iter().map(|v| v.code).collect();
                let hint = if current.is_empty() { "en".to_string() } else { current.join(",") };
                out("Type the language codes separated by commas, for example: en,es or en-GB,es-MX,fr");
                let Some(line) = prompt(&format!("Languages [{hint}]: ")) else { return 0 };
                let line = if line.is_empty() { hint } else { line };
                let codes: Vec<String> = line.split([',', ';', ' ']).filter(|s| !s.is_empty()).map(String::from).collect();
                report(core::install(&codes, out).map(|_| ()), out);
            }
            "2" => {
                let Some(q) = prompt("Search for (name or code): ") else { return 0 };
                let q = q.to_lowercase();
                let hits: Vec<_> = core::all_languages().into_iter().filter(|l| l.label.to_lowercase().contains(&q)).collect();
                if hits.is_empty() {
                    out("No language matches.");
                }
                for l in hits {
                    out(&format!("  {}", l.label));
                }
            }
            "3" => {
                let Some(yes) = prompt("Remove Clipboard Voice and all its voices? (y/N): ") else { return 0 };
                if yes.eq_ignore_ascii_case("y") || yes.eq_ignore_ascii_case("yes") {
                    report(core::uninstall(out), out);
                }
            }
            "4" => return 0,
            _ => out("Invalid option."),
        }
    }
}
