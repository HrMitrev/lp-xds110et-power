use std::env;
use std::fs;
use std::path::Path;
use twemoji_assets::png::PngTwemojiAsset;

fn main() {
    println!("cargo::rerun-if-changed=build.rs");

    let out_dir = env::var("OUT_DIR").expect("OUT_DIR not set");
    let dest_path = Path::new(&out_dir).join("icon.png");

    let emoji_asset =
        PngTwemojiAsset::from_emoji("⚡").expect("Emoji '⚡' not found in Twemoji assets");

    fs::write(&dest_path, **emoji_asset).expect("Failed to write icon.png");
}
