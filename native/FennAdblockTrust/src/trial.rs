//! Development feed signing and Windows connection-cost checks. No production keys.
use super::*;
use aws_lc_rs::signature::{Ed25519KeyPair, KeyPair};
use serde_json::{json, Value};
use std::{ffi::c_void, io::Write, ptr};
#[repr(C)]
struct Blob { len: u32, data: *mut u8 }
#[link(name="crypt32")]
unsafe extern "system" {
    fn CryptProtectData(input:*const Blob, description:*const u16, entropy:*const Blob, reserved:*mut c_void, prompt:*mut c_void, flags:u32, output:*mut Blob)->i32;
    fn CryptUnprotectData(input:*const Blob, description:*mut *mut u16, entropy:*const Blob, reserved:*mut c_void, prompt:*mut c_void, flags:u32, output:*mut Blob)->i32;
}
#[link(name="kernel32")]
unsafe extern "system" { fn LocalFree(ptr:*mut c_void)->*mut c_void; }
fn protect(bytes:&[u8], decrypt:bool)->Result<Vec<u8>> {
    let input=Blob {len:bytes.len().try_into()?,data:bytes.as_ptr() as *mut u8};
    let mut output=Blob {len:0,data:ptr::null_mut()};
    let ok=unsafe { if decrypt { CryptUnprotectData(&input,ptr::null_mut(),ptr::null(),ptr::null_mut(),ptr::null_mut(),1,&mut output) }
        else { CryptProtectData(&input,ptr::null(),ptr::null(),ptr::null_mut(),ptr::null_mut(),1,&mut output) } };
    if ok==0 { return Err(std::io::Error::last_os_error().into()); }
    let result=unsafe { std::slice::from_raw_parts(output.data,output.len as usize).to_vec() };
    unsafe { LocalFree(output.data.cast()); }
    Ok(result)
}
fn hex(bytes:&[u8])->String { bytes.iter().map(|b|format!("{b:02x}")).collect() }
struct Key {pair:Ed25519KeyPair,public:Value,id:String}
impl Key {
    fn from(bytes:&[u8])->Result<Self> {
        let pair=Ed25519KeyPair::from_pkcs8(bytes).map_err(|_|"invalid trial key")?;
        let public=json!({"keytype":"ed25519","scheme":"ed25519","keyval":{"public":hex(pair.public_key().as_ref())}});
        let id=hex_hash(&serde_json::to_vec(&public)?);
        Ok(Self {pair,public,id})
    }
    fn sign(&self,value:Value)->Result<Vec<u8>> {
        // Only ASCII keys/values and integers are authored here: sorted compact
        // serde JSON is TUF canonical JSON for this constrained metadata.
        let bytes=serde_json::to_vec(&value)?;
        if !bytes.is_ascii() { return Err("metadata must be ASCII".into()); }
        Ok(serde_json::to_vec(&json!({"signed":value,"signatures":[{"keyid":self.id,"sig":hex(self.pair.sign(&bytes).as_ref())}]}))?)
    }
}
pub fn key(path:&Path, expires:&str)->Result<Value> {
    let bytes=Ed25519KeyPair::generate_pkcs8(&aws_lc_rs::rand::SystemRandom::new()).map_err(|_|"key generation failed")?;
    let key=Key::from(bytes.as_ref())?;
    let role=json!({"keyids":[key.id],"threshold":1});
    let root=key.sign(json!({"_type":"root","spec_version":"1.0.0","version":1,"expires":expires,"consistent_snapshot":true,
        "keys":{key.id.clone():key.public},"roles":{"root":role,"targets":role,"snapshot":role,"timestamp":role}}))?;
    let sealed=protect(bytes.as_ref(),false)?;
    let mut file=fs::OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&sealed)?;file.sync_all()?;
    let root_path=path.with_extension("root.json");
    let mut file=fs::OpenOptions::new().write(true).create_new(true).open(&root_path)?;
    file.write_all(&root)?;file.sync_all()?;
    Ok(json!({"root":root_path,"keyid":key.id,"protection":"Windows DPAPI current user; trial only"}))
}
pub fn publish(path:&Path,bundle:&Path,output:&Path,version:u64,expires:&str)->Result<Value> {
    if version==0 {return Err("version must be positive".into());}
    let mut raw=protect(&read(path,65536)?,true)?;
    let parsed=Key::from(&raw);raw.fill(0);let key=parsed?;
    fs::create_dir(output)?;fs::create_dir(output.join("metadata"))?;fs::create_dir(output.join("targets"))?;
    let mut entries=serde_json::Map::new();
    for (name,limit) in TARGETS {
        let bytes=read(&bundle.join(name),limit)?;let digest=hex_hash(&bytes);
        fs::write(output.join("targets").join(format!("{digest}.{name}")),&bytes)?;
        entries.insert(name.into(),json!({"length":bytes.len(),"hashes":{"sha256":digest}}));
    }
    let targets=key.sign(json!({"_type":"targets","spec_version":"1.0.0","version":version,"expires":expires,"targets":entries}))?;
    let meta=|b:&[u8]|json!({"version":version,"length":b.len(),"hashes":{"sha256":hex_hash(b)}});
    let snapshot=key.sign(json!({"_type":"snapshot","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"targets.json":meta(&targets)}}))?;
    let timestamp=key.sign(json!({"_type":"timestamp","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"snapshot.json":meta(&snapshot)}}))?;
    let root=read(&path.with_extension("root.json"),META)?;
    fs::write(output.join("metadata/1.root.json"),&root)?;
    fs::write(output.join("metadata").join(format!("{version}.targets.json")),targets)?;
    fs::write(output.join("metadata").join(format!("{version}.snapshot.json")),snapshot)?;
    fs::write(output.join("metadata/timestamp.json"),timestamp)?;
    Ok(json!({"version":version,"expires":expires,"root_sha256":hex_hash(&root)}))
}
#[repr(C)]
struct Guid {a:u32,b:u16,c:u16,d:[u8;8]}
#[link(name="ole32")]
unsafe extern "system" {
    fn CoInitializeEx(reserved:*mut c_void,flags:u32)->i32;
    fn CoUninitialize();
    fn CoCreateInstance(clsid:*const Guid,outer:*mut c_void,context:u32,iid:*const Guid,result:*mut *mut c_void)->i32;
}
#[repr(C)]
struct CostVtable {
    query:usize,add:usize,release:unsafe extern "system" fn(*mut c_void)->u32,
    cost:unsafe extern "system" fn(*mut c_void,*mut u32,*const c_void)->i32,
}
pub fn network_cost()->Value {
    // SDK netlistmgr.h: INetworkCostManager/GetCost, preferred machine route.
    unsafe {
        let init=CoInitializeEx(ptr::null_mut(),0);
        if init<0 {return json!({"allowed":false,"error":"COM initialization"});}
        let cls=Guid {a:0xdcb00c01,b:0x570f,c:0x4a9b,d:[0x8d,0x69,0x19,0x9f,0xdb,0xa5,0x72,0x3b]};
        let iid=Guid {a:0xdcb00008,..cls};let mut object=ptr::null_mut();
        let hr=CoCreateInstance(&cls,ptr::null_mut(),23,&iid,&mut object);
        let mut flags=0;
        let ok=if hr>=0 && !object.is_null() {
            let vtable=&**(object as *const *const CostVtable);
            let result=(vtable.cost)(object,&mut flags,ptr::null());
            (vtable.release)(object);result>=0
        } else {false};
        CoUninitialize();
        // Unknown, fixed, variable, roaming, over-limit and congestion defer.
        json!({"allowed":ok && flags==1,"flags":flags})
    }
}
