//! Child-only reconnect descriptor. It carries identity, never a path or command.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellReconnectDescriptor {
    pub peer: u32,
    pub generation: u32,
    pub nonce: [u16; 32],
}
fn parse(input: &[u16]) -> Option<ShellReconnectDescriptor> {
    if input.len()>64 { return None; }
    let text=String::from_utf16(input).ok()?;
    let parts: Vec<_>=text.split(':').collect();
    if parts.len()!=4 || parts[0]!="1" || parts[3].len()!=32 ||
        !parts[3].bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {return None;}
    let number=|value:&str| -> Option<u32> {
        if value.starts_with('0') || !value.bytes().all(|b| b.is_ascii_digit()) {return None;}
        value.parse().ok().filter(|&n|n!=0)
    };
    let mut nonce=[0;32];for (to,from) in nonce.iter_mut().zip(parts[3].encode_utf16()){*to=from;}
    Some(ShellReconnectDescriptor{peer:number(parts[1])?,generation:number(parts[2])?,nonce})
}
/// # Safety
/// Input is readable for length units; output is writable and disjoint.
#[no_mangle]
pub unsafe extern "C" fn shell_reconnect_parse(input:*const u16,length:usize,output:*mut ShellReconnectDescriptor)->bool {
    if input.is_null() || output.is_null() || length>64{return false;}
    let parsed=std::panic::catch_unwind(||parse(std::slice::from_raw_parts(input,length)));
    if let Ok(Some(value))=parsed {*output=value;true} else {false}
}
#[cfg(test)] mod tests {
    use super::*;
    #[test] fn descriptor_is_bounded_and_has_no_path_or_command() {
        let valid="1:42:4294967295:1234567890abcdef1234567890abcdef";
        assert_eq!(parse(&valid.encode_utf16().collect::<Vec<_>>()).unwrap().peer,42);
        for bad in [valid.replace("42", "0"),valid.replace("1:42:","1:042:"),valid.replace("1:42:","2:42:"),
            valid.replace("1:42:","1:+42:"),valid.replace("4294967295","4294967296"),format!("{valid}:x"),
            valid.replace("abcdef","ABCDEF"),"C:\\fenn.exe".into()] {
            assert!(parse(&bad.encode_utf16().collect::<Vec<_>>()).is_none(),"{bad}");
        }
    }
}
