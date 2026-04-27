/*
 * fbinst UD read-only mount (WinFsp / Dokan). Used by wim.exe --ud.
 */
#ifndef UD_H
#define UD_H

BOOL WINAPI wim_console_ctrl(DWORD dwCtrlType);
int ud_mount_main(const wchar_t *ud_source, wchar_t *mount_path, int mount_backend);

#endif
