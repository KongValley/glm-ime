//! 管道安全属性：SDDL 字符串生成 DACL。
//! 背景：launcher 若由提权 shell 启动，默认 DACL（完整令牌）会拒绝非提权进程
//! （UAC 过滤令牌）访问管道 —— TIP 宿主连接被拒（ERROR_ACCESS_DENIED）。
//! 实现：ConvertStringSecurityDescriptorToSecurityDescriptorW("D:(A;;GRGW;;;WD)")
//! —— 允许 Everyone 通用读写。返回的自相对 SD 由系统分配，进程内持续有效。

use windows_sys::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
use windows_sys::Win32::Security::{SECURITY_ATTRIBUTES, PSECURITY_DESCRIPTOR};

const SDDL_REVISION_1: u32 = 1;
// D: DACL；(A;;GRGW;;;WD)：Allow，GenericRead|GenericWrite，World(Everyone)
const SDDL_PIPE: &str = "D:(A;;GRGW;;;WD)";

pub struct PipeSecurity {
    pub sa: SECURITY_ATTRIBUTES,
    sd_ptr: PSECURITY_DESCRIPTOR,
}

impl PipeSecurity {
    pub fn new() -> Self {
        unsafe {
            let mut sddl: Vec<u16> = SDDL_PIPE.encode_utf16().collect();
            sddl.push(0);
            let mut sd_ptr: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
            let ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut sd_ptr,
                std::ptr::null_mut(),
            );
            let sa = if ok != 0 && !sd_ptr.is_null() {
                SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: sd_ptr,
                    bInheritHandle: 0,
                }
            } else {
                // SDDL 失败（理论上不会）：退回 NULL SECURITY_ATTRIBUTES（系统默认 DACL）
                eprintln!("[acl] SDDL failed err={}", ::std::io::Error::last_os_error());
                SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: std::ptr::null_mut(),
                    bInheritHandle: 0,
                }
            };
            PipeSecurity { sa, sd_ptr }
        }
    }
}
