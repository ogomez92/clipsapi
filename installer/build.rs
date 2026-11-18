fn main() {
    // The DLLs are embedded with include_bytes!; rebuild when they change.
    println!("cargo:rerun-if-changed=../ClipboardVoice/x64/Release/ClipboardVoice.dll");
    println!("cargo:rerun-if-changed=../ClipboardVoice/x86/Release/ClipboardVoice.dll");
    println!("cargo:rerun-if-changed=../vendor/prism/x64/prism.dll");
    println!("cargo:rerun-if-changed=../vendor/prism/x86/prism.dll");
    println!("cargo:rerun-if-changed=app.rc");
    println!("cargo:rerun-if-changed=app.manifest");
    if std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default() == "windows" {
        embed_resource::compile("app.rc", embed_resource::NONE)
            .manifest_optional()
            .expect("failed to embed app.rc");
    }
}
