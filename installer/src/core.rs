//! Install / uninstall logic shared by the GUI and the CLI.
//!
//! The installer carries both engine DLLs. Installing copies them to
//! Program Files, registers them with regsvr32 (`/i:<languages>` makes the DLL
//! create one SAPI voice per language), copies this exe next to them and adds an
//! entry to Settings > Apps so the voice can be changed or removed later.

use std::fs;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use winreg::enums::*;
use winreg::RegKey;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");

static DLL_X64: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../ClipboardVoice/x64/Release/ClipboardVoice.dll"));
static DLL_X86: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../ClipboardVoice/x86/Release/ClipboardVoice.dll"));

const TOKENS_KEY: &str = r"SOFTWARE\Microsoft\Speech\Voices\Tokens";
const TOKEN_PREFIX: &str = "ClipboardVoice";
const UNINSTALL_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ClipboardVoice";
const CLSID: &str = "{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}";
const EXE_NAME: &str = "ClipboardVoiceInstaller.exe";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

// ---------- Languages ----------

/// A language Windows knows and SAPI can use (it has a Windows language ID).
#[derive(Clone, Debug)]
pub struct Language {
    /// Locale name passed to the DLL: "es", "es-MX", ...
    pub code: String,
    /// "Spanish (Mexico), español (México), es-MX"
    pub label: String,
    /// "Spanish (Mexico)"
    pub english: String,
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn from_wide(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}

fn locale_info(name: &str, kind: u32) -> Option<String> {
    use windows_sys::Win32::Globalization::GetLocaleInfoEx;
    let w = wide(name);
    let mut buf = [0u16; 256];
    let n = unsafe { GetLocaleInfoEx(w.as_ptr(), kind as _, buf.as_mut_ptr(), buf.len() as i32) };
    (n > 0).then(|| from_wide(&buf)).filter(|s| !s.is_empty())
}

/// "es" -> "es-ES"; a specific name comes back unchanged. `None` when Windows
/// doesn't know it.
fn resolve_specific(code: &str) -> Option<String> {
    use windows_sys::Win32::Globalization::ResolveLocaleName;
    let w = wide(code);
    let mut buf = [0u16; 85];
    let n = unsafe { ResolveLocaleName(w.as_ptr(), buf.as_mut_ptr(), buf.len() as i32) };
    (n > 0).then(|| from_wide(&buf)).filter(|s| !s.is_empty())
}

fn lcid_of(name: &str) -> u32 {
    use windows_sys::Win32::Globalization::LocaleNameToLCID;
    let w = wide(name);
    unsafe { LocaleNameToLCID(w.as_ptr(), 0) }
}

fn usable_lcid(lcid: u32) -> bool {
    // 0 = unknown; 0x1000 / 0x0C00 = locales without a real language ID, which SAPI can't express.
    lcid != 0 && lcid != 0x1000 && lcid != 0x0C00
}

/// Checks a locale name the way the DLL will and returns it normalised
/// ("ES_mx" -> "es-MX"), or an error message.
pub fn validate_code(code: &str) -> Result<String, String> {
    let code = code.trim().replace('_', "-");
    if code.is_empty() {
        return Err("empty language code".into());
    }
    let specific = resolve_specific(&code)
        .ok_or_else(|| format!("'{code}' is not a language Windows knows (try codes like en, es, fr, es-MX)"))?;
    if !usable_lcid(lcid_of(&specific)) {
        return Err(format!("'{code}' has no Windows language ID, so SAPI voices can't use it"));
    }
    if code.contains('-') {
        // Use Windows' own spelling of the name.
        Ok(specific)
    } else {
        Ok(code.to_ascii_lowercase())
    }
}

/// Every language SAPI can use: each language on its own ("Spanish") followed
/// by its regional variants ("Spanish (Mexico)"), sorted by English name.
pub fn all_languages() -> Vec<Language> {
    use windows_sys::Win32::Foundation::{BOOL, LPARAM};
    use windows_sys::Win32::Globalization::EnumSystemLocalesEx;

    unsafe extern "system" fn collect(name: *const u16, _flags: u32, lparam: LPARAM) -> BOOL {
        let names = &mut *(lparam as *mut Vec<String>);
        let mut len = 0;
        while *name.add(len) != 0 {
            len += 1;
        }
        names.push(String::from_utf16_lossy(std::slice::from_raw_parts(name, len)));
        1
    }

    let mut names: Vec<String> = Vec::new();
    // LOCALE_WINDOWS (1): locales that ship with Windows, neutral and specific.
    unsafe {
        EnumSystemLocalesEx(Some(collect), 1, &mut names as *mut _ as LPARAM, std::ptr::null());
    }

    const LOCALE_SENGLISHDISPLAYNAME: u32 = 0x72;
    const LOCALE_SNATIVEDISPLAYNAME: u32 = 0x73;

    let mut out: Vec<Language> = names
        .into_iter()
        .filter(|n| !n.is_empty() && !n.contains('_'))
        .filter(|n| resolve_specific(n).is_some_and(|s| usable_lcid(lcid_of(&s))))
        .filter_map(|code| {
            let english = locale_info(&code, LOCALE_SENGLISHDISPLAYNAME)?;
            let native = locale_info(&code, LOCALE_SNATIVEDISPLAYNAME);
            let label = match native {
                Some(n) if !n.eq_ignore_ascii_case(&english) => format!("{english}, {n}, {code}"),
                _ => format!("{english}, {code}"),
            };
            let code = if code.contains('-') { code } else { code.to_ascii_lowercase() };
            Some(Language { code, label, english })
        })
        .collect();
    out.sort_by(|a, b| a.english.to_lowercase().cmp(&b.english.to_lowercase()).then(a.code.cmp(&b.code)));
    out.dedup_by(|a, b| a.code.eq_ignore_ascii_case(&b.code));
    out
}

/// Registry key the DLL uses for a language (mirrors ResolveVoiceLanguage in the DLL).
fn token_key_for(code: &str) -> Option<String> {
    let specific = resolve_specific(code)?;
    if specific.eq_ignore_ascii_case("en-US") {
        Some(TOKEN_PREFIX.to_string())
    } else {
        let id = if code.contains('-') { specific } else { code.to_string() };
        Some(format!("{TOKEN_PREFIX}_{}", id.to_lowercase()))
    }
}

// ---------- Current state ----------

pub struct InstalledVoice {
    /// Language code as given at install time ("en" for the original English voice).
    pub code: String,
    /// Voice name as SAPI programs list it.
    pub name: String,
}

fn hklm() -> RegKey {
    RegKey::predef(HKEY_LOCAL_MACHINE)
}

fn token_names(view: u32) -> Vec<String> {
    let Ok(tokens) = hklm().open_subkey_with_flags(TOKENS_KEY, KEY_READ | view) else {
        return Vec::new();
    };
    tokens
        .enum_keys()
        .filter_map(Result::ok)
        .filter(|k| is_our_token(k))
        .collect()
}

/// "ClipboardVoice" or "ClipboardVoice_<code>" (same rule as the DLL).
fn is_our_token(key: &str) -> bool {
    let n = TOKEN_PREFIX.len();
    match key.get(..n) {
        Some(head) if head.eq_ignore_ascii_case(TOKEN_PREFIX) => key.len() == n || (key.as_bytes()[n] == b'_' && key.len() > n + 1),
        _ => false,
    }
}

/// Voices currently installed (as seen by 64-bit programs).
pub fn installed_voices() -> Vec<InstalledVoice> {
    let mut voices: Vec<InstalledVoice> = token_names(KEY_WOW64_64KEY)
        .into_iter()
        .map(|key| {
            let name = hklm()
                .open_subkey_with_flags(format!(r"{TOKENS_KEY}\{key}"), KEY_READ | KEY_WOW64_64KEY)
                .and_then(|k| k.get_value::<String, _>(""))
                .unwrap_or_else(|_| key.clone());
            let code = if key.eq_ignore_ascii_case(TOKEN_PREFIX) {
                "en".to_string()
            } else {
                normalise_case(&key[TOKEN_PREFIX.len() + 1..])
            };
            InstalledVoice { code, name }
        })
        .collect();
    voices.sort_by(|a, b| a.name.cmp(&b.name));
    voices
}

/// "es-mx" -> "es-MX" (Windows' spelling), "es" stays "es".
fn normalise_case(code: &str) -> String {
    if code.contains('-') {
        resolve_specific(code).unwrap_or_else(|| code.to_string())
    } else {
        code.to_string()
    }
}

pub fn is_installed() -> bool {
    !installed_voices().is_empty()
        || hklm().open_subkey_with_flags(UNINSTALL_KEY, KEY_READ | KEY_WOW64_64KEY).is_ok()
}

// ---------- Install ----------

struct Target {
    arch: &'static str,
    dir: PathBuf,
    regsvr32: PathBuf,
    dll: &'static [u8],
    view: u32,
}

fn targets() -> Vec<Target> {
    let windir = PathBuf::from(std::env::var_os("SystemRoot").unwrap_or_else(|| r"C:\Windows".into()));
    let pf64 = PathBuf::from(std::env::var_os("ProgramW6432").or_else(|| std::env::var_os("ProgramFiles")).unwrap_or_else(|| r"C:\Program Files".into()));
    let pf32 = PathBuf::from(std::env::var_os("ProgramFiles(x86)").unwrap_or_else(|| r"C:\Program Files (x86)".into()));
    vec![
        Target { arch: "64-bit", dir: pf64.join("ClipboardVoice"), regsvr32: windir.join(r"System32\regsvr32.exe"), dll: DLL_X64, view: KEY_WOW64_64KEY },
        Target { arch: "32-bit", dir: pf32.join("ClipboardVoice"), regsvr32: windir.join(r"SysWOW64\regsvr32.exe"), dll: DLL_X86, view: KEY_WOW64_32KEY },
    ]
}

fn main_dir() -> PathBuf {
    targets().remove(0).dir
}

/// Writes `data` to `dest`. A DLL that a running program (game, screen reader)
/// has loaded can't be overwritten but can be renamed, so move it aside; that
/// program keeps using the old copy until it restarts.
fn write_replacing(dest: &Path, data: &[u8], log: &mut dyn FnMut(&str)) -> Result<(), String> {
    if fs::read(dest).is_ok_and(|old| old == data) {
        return Ok(());
    }
    match fs::write(dest, data) {
        Ok(()) => Ok(()),
        Err(first) if dest.exists() => {
            let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            let aside = dest.with_extension(format!("{stamp}.old"));
            fs::rename(dest, &aside).map_err(|_| format!("could not replace {}: {first}", dest.display()))?;
            log(&format!("{} is in use; programs that have it open get the new version after they restart.", dest.display()));
            fs::write(dest, data).map_err(|e| format!("could not write {}: {e}", dest.display()))
        }
        Err(e) => Err(format!("could not write {}: {e}", dest.display())),
    }
}

fn remove_old_copies(dir: &Path) {
    if let Ok(entries) = fs::read_dir(dir) {
        for e in entries.flatten() {
            if e.path().extension().is_some_and(|x| x == "old") {
                let _ = fs::remove_file(e.path());
            }
        }
    }
}

fn regsvr32(t: &Target, args: &[&str]) -> Result<(), String> {
    let status = Command::new(&t.regsvr32)
        .args(args)
        .creation_flags(CREATE_NO_WINDOW)
        .status()
        .map_err(|e| format!("could not run {}: {e}", t.regsvr32.display()))?;
    match status.code() {
        Some(0) => Ok(()),
        Some(3) => Err(format!("Windows could not load the {} DLL. An antivirus may have blocked or removed it.", t.arch)),
        Some(4) => Err(format!("the {} DLL has no registration entry point", t.arch)),
        Some(5) => Err(format!("registering the {} COM class failed", t.arch)),
        Some(c) => Err(format!("registering the {} voices failed (regsvr32 code {c})", t.arch)),
        None => Err("regsvr32 was terminated".into()),
    }
}

/// Installs (or reconfigures) Clipboard Voice with exactly these languages.
pub fn install(codes: &[String], log: &mut dyn FnMut(&str)) -> Result<Vec<InstalledVoice>, String> {
    let mut langs: Vec<String> = Vec::new();
    for c in codes {
        let v = validate_code(c)?;
        if !langs.iter().any(|l| l.eq_ignore_ascii_case(&v)) {
            langs.push(v);
        }
    }
    if langs.is_empty() {
        return Err("choose at least one language".into());
    }
    let lang_arg = langs.join(",");
    log(&format!("Installing Clipboard Voice {VERSION} for: {lang_arg}"));

    for t in targets() {
        fs::create_dir_all(&t.dir).map_err(|e| format!("could not create {}: {e}", t.dir.display()))?;
        remove_old_copies(&t.dir);
        let dll = t.dir.join("ClipboardVoice.dll");
        write_replacing(&dll, t.dll, log)?;
        let dll_s = dll.to_string_lossy().to_string();
        regsvr32(&t, &["/s", &format!("/i:{lang_arg}"), &dll_s])?;
        log(&format!("Registered the {} voice in {}.", t.arch, t.dir.display()));
    }

    // Check the voices exist for both 64-bit and 32-bit programs.
    let mut expected: Vec<String> = langs.iter().filter_map(|l| token_key_for(l)).collect();
    expected.sort();
    expected.dedup();
    for t in targets() {
        let present = token_names(t.view);
        let missing: Vec<&String> = expected.iter().filter(|k| !present.iter().any(|p| p.eq_ignore_ascii_case(k))).collect();
        if !missing.is_empty() {
            return Err(format!("after registering, {} programs are missing voices: {missing:?}", t.arch));
        }
    }

    if let Err(e) = register_uninstaller(log) {
        log(&format!("Warning: could not add Clipboard Voice to Settings > Apps: {e}"));
    }

    let voices = installed_voices();
    for v in &voices {
        log(&format!("Voice ready: {} ({})", v.name, v.code));
    }
    Ok(voices)
}

/// Copies this exe into the install folder and lists Clipboard Voice in Settings > Apps,
/// where "Modify" reopens this installer and "Uninstall" removes everything.
fn register_uninstaller(log: &mut dyn FnMut(&str)) -> Result<(), String> {
    let dir = main_dir();
    let exe = dir.join(EXE_NAME);
    let me = std::env::current_exe().map_err(|e| e.to_string())?;
    let same = fs::canonicalize(&me).ok() == fs::canonicalize(&exe).ok();
    if !same {
        let data = fs::read(&me).map_err(|e| format!("could not read {}: {e}", me.display()))?;
        write_replacing(&exe, &data, log)?;
    }
    let exe_s = exe.to_string_lossy();
    let (key, _) = hklm()
        .create_subkey_with_flags(UNINSTALL_KEY, KEY_WRITE | KEY_WOW64_64KEY)
        .map_err(|e| e.to_string())?;
    let set = |name: &str, value: &str| key.set_value(name, &value.to_string()).map_err(|e| e.to_string());
    set("DisplayName", "Clipboard Voice")?;
    set("DisplayVersion", VERSION)?;
    set("Publisher", "Clipboard Voice")?;
    set("InstallLocation", &dir.to_string_lossy())?;
    set("DisplayIcon", &exe_s)?;
    set("UninstallString", &format!("\"{exe_s}\" --uninstall"))?;
    set("QuietUninstallString", &format!("\"{exe_s}\" --cli --uninstall"))?;
    set("ModifyPath", &format!("\"{exe_s}\""))?;
    key.set_value("NoRepair", &1u32).map_err(|e| e.to_string())?;
    let size_kb = ((DLL_X64.len() + DLL_X86.len()) as u64 + fs::metadata(&exe).map(|m| m.len()).unwrap_or(0)) / 1024;
    key.set_value("EstimatedSize", &(size_kb as u32)).map_err(|e| e.to_string())?;
    Ok(())
}

// ---------- Uninstall ----------

pub fn uninstall(log: &mut dyn FnMut(&str)) -> Result<(), String> {
    let me = std::env::current_exe().ok().and_then(|p| fs::canonicalize(p).ok());
    let mut self_dir: Option<PathBuf> = None;

    for t in targets() {
        let dll = t.dir.join("ClipboardVoice.dll");
        if dll.exists() {
            let dll_s = dll.to_string_lossy().to_string();
            match regsvr32(&t, &["/s", "/u", &dll_s]) {
                Ok(()) => log(&format!("Unregistered the {} voice.", t.arch)),
                Err(e) => log(&format!("Warning: {e}; cleaning the registry directly.")),
            }
        }
        // Also covers a DLL removed by antivirus before uninstalling.
        let root = hklm();
        if let Ok(tokens) = root.open_subkey_with_flags(TOKENS_KEY, KEY_ALL_ACCESS | t.view) {
            for name in token_names(t.view) {
                if tokens.delete_subkey_all(&name).is_ok() {
                    log(&format!("Removed voice {name} ({}).", t.arch));
                }
            }
        }
        if let Ok(classes) = root.open_subkey_with_flags(r"SOFTWARE\Classes", KEY_ALL_ACCESS | t.view) {
            let _ = classes.delete_subkey_all(format!(r"CLSID\{CLSID}"));
            let _ = classes.delete_subkey_all("ClipboardVoice.Engine");
        }

        if t.dir.exists() {
            let running_from_here = me.as_ref().is_some_and(|m| fs::canonicalize(&t.dir).is_ok_and(|d| m.starts_with(d)));
            if running_from_here {
                self_dir = Some(t.dir.clone());
                // Everything but ourselves now; the folder after we exit.
                if let Ok(entries) = fs::read_dir(&t.dir) {
                    for e in entries.flatten() {
                        if fs::canonicalize(e.path()).ok() != me {
                            let _ = fs::remove_file(e.path());
                        }
                    }
                }
            } else {
                match fs::remove_dir_all(&t.dir) {
                    Ok(()) => log(&format!("Removed {}.", t.dir.display())),
                    Err(e) => log(&format!(
                        "Could not remove {} ({e}). A running program may still have the voice loaded; delete the folder after closing it.",
                        t.dir.display()
                    )),
                }
            }
        }
    }

    let _ = hklm()
        .open_subkey_with_flags(r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", KEY_ALL_ACCESS | KEY_WOW64_64KEY)
        .and_then(|k| k.delete_subkey_all("ClipboardVoice"));

    if let Some(dir) = self_dir {
        // Started from Settings > Apps: this exe is in the folder; delete it once we have exited.
        let cmd = format!("ping -n 3 127.0.0.1 >nul & rmdir /s /q \"{}\"", dir.display());
        let _ = Command::new("cmd.exe").raw_arg("/c").raw_arg(&cmd).creation_flags(CREATE_NO_WINDOW).spawn();
        log(&format!("{} will be removed when this window closes.", dir.display()));
    }

    if token_names(KEY_WOW64_64KEY).is_empty() && token_names(KEY_WOW64_32KEY).is_empty() {
        log("Clipboard Voice uninstalled.");
        Ok(())
    } else {
        Err("some voice registry entries could not be removed".into())
    }
}
