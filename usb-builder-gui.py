#!/usr/bin/env python3
"""
xemu Appliance USB Builder
Standalone GUI tool - no Claude required
"""

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext, simpledialog
import subprocess
import threading
import os
import re

class PasswordDialog(simpledialog.Dialog):
    """Custom password dialog that masks input"""
    def body(self, master):
        ttk.Label(master, text="Enter sudo password:").grid(row=0, sticky=tk.W)
        self.password_entry = ttk.Entry(master, show="*", width=30)
        self.password_entry.grid(row=1, pady=5)
        return self.password_entry

    def apply(self):
        self.result = self.password_entry.get()


class USBBuilder:
    def __init__(self, root):
        self.root = root
        self.root.title("xemu Appliance USB Builder")
        self.root.geometry("700x600")
        self.root.resizable(True, True)

        self.source_path = "/home/doug/xemu-appliance/rootfs"
        self.root_label = "XEMU_ROOT"
        self.building = False
        self.current_step = 0
        self.total_steps = 10
        self.sudo_password = None

        # Prompt for sudo password immediately
        self.get_sudo_password()

        self.create_widgets()
        self.refresh_devices()

    def get_sudo_password(self):
        """Prompt for sudo password and verify it works"""
        dialog = PasswordDialog(self.root, title="Sudo Authentication")
        if dialog.result:
            self.sudo_password = dialog.result
            # Verify password works
            try:
                result = subprocess.run(
                    ['sudo', '-S', '-v'],
                    input=self.sudo_password + '\n',
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode != 0:
                    messagebox.showerror("Error", "Invalid sudo password")
                    self.root.quit()
            except Exception as e:
                messagebox.showerror("Error", f"Failed to verify sudo: {e}")
                self.root.quit()
        else:
            messagebox.showwarning("Cancelled", "Sudo password required to build USB")
            self.root.quit()

    def create_widgets(self):
        # Device selection frame
        device_frame = ttk.LabelFrame(self.root, text="1. Select USB Device", padding=10)
        device_frame.pack(fill=tk.X, padx=10, pady=5)

        self.device_var = tk.StringVar()
        self.device_combo = ttk.Combobox(device_frame, textvariable=self.device_var, state="readonly", width=60)
        self.device_combo.pack(side=tk.LEFT, padx=5)

        refresh_btn = ttk.Button(device_frame, text="Refresh", command=self.refresh_devices)
        refresh_btn.pack(side=tk.LEFT, padx=5)

        # Warning label
        warn_frame = ttk.Frame(self.root)
        warn_frame.pack(fill=tk.X, padx=10, pady=5)
        warn_label = ttk.Label(warn_frame, text="⚠️  WARNING: Selected device will be COMPLETELY ERASED!",
                               foreground="red", font=("", 12, "bold"))
        warn_label.pack()

        # Source path frame
        source_frame = ttk.LabelFrame(self.root, text="2. Source Rootfs", padding=10)
        source_frame.pack(fill=tk.X, padx=10, pady=5)

        self.source_var = tk.StringVar(value=self.source_path)
        source_entry = ttk.Entry(source_frame, textvariable=self.source_var, width=70)
        source_entry.pack(side=tk.LEFT, padx=5)

        # Progress frame
        progress_frame = ttk.LabelFrame(self.root, text="3. Progress", padding=10)
        progress_frame.pack(fill=tk.X, padx=10, pady=5)

        self.step_label = ttk.Label(progress_frame, text="Ready", font=("", 11))
        self.step_label.pack(anchor=tk.W)

        self.progress = ttk.Progressbar(progress_frame, length=650, mode='determinate')
        self.progress.pack(fill=tk.X, pady=5)

        self.substep_label = ttk.Label(progress_frame, text="", foreground="gray")
        self.substep_label.pack(anchor=tk.W)

        # Log frame
        log_frame = ttk.LabelFrame(self.root, text="4. Log Output (Ctrl+C to copy)", padding=10)
        log_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=15, font=("Courier", 9))
        self.log_text.pack(fill=tk.BOTH, expand=True)
        # Enable text selection and copying
        self.log_text.bind("<Control-c>", lambda e: self.copy_log())
        self.log_text.bind("<Control-a>", lambda e: self.select_all_log())

        # Buttons frame
        btn_frame = ttk.Frame(self.root)
        btn_frame.pack(fill=tk.X, padx=10, pady=10)

        self.build_btn = tk.Button(btn_frame, text="  BUILD USB  ", command=self.start_build,
                                   bg="#4CAF50", fg="white", font=("", 12, "bold"),
                                   activebackground="#45a049", activeforeground="white")
        self.build_btn.pack(side=tk.LEFT, padx=5, ipady=5)

        self.cancel_btn = tk.Button(btn_frame, text="Cancel", command=self.cancel_build, state=tk.DISABLED)
        self.cancel_btn.pack(side=tk.LEFT, padx=5)

        quit_btn = tk.Button(btn_frame, text="Quit", command=self.root.quit)
        quit_btn.pack(side=tk.RIGHT, padx=5)

    def copy_log(self):
        """Copy selected text to clipboard"""
        try:
            text = self.log_text.get(tk.SEL_FIRST, tk.SEL_LAST)
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
        except tk.TclError:
            # No selection, copy all
            text = self.log_text.get(1.0, tk.END)
            self.root.clipboard_clear()
            self.root.clipboard_append(text)

    def select_all_log(self):
        """Select all text in log"""
        self.log_text.tag_add(tk.SEL, "1.0", tk.END)
        return "break"

    def log(self, msg):
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)
        self.root.update_idletasks()

    def set_step(self, step_num, description):
        self.current_step = step_num
        self.step_label.config(text=f"[{step_num}/{self.total_steps}] {description}")
        self.progress['value'] = (step_num / self.total_steps) * 100
        self.log(f"\n=== Step {step_num}: {description} ===")
        self.root.update_idletasks()

    def set_substep(self, msg):
        self.substep_label.config(text=msg)
        self.root.update_idletasks()

    def refresh_devices(self):
        """Get list of removable block devices"""
        devices = []
        try:
            result = subprocess.run(['lsblk', '-dpno', 'NAME,SIZE,MODEL,TRAN'],
                                   capture_output=True, text=True)
            for line in result.stdout.strip().split('\n'):
                if not line:
                    continue
                parts = line.split(None, 3)
                if len(parts) >= 2:
                    name = parts[0]
                    size = parts[1]
                    model = parts[2] if len(parts) > 2 else "Unknown"
                    tran = parts[3] if len(parts) > 3 else ""

                    # Skip non-USB and system drives
                    if 'usb' in tran.lower() or (name.startswith('/dev/sd') and name != '/dev/sda'):
                        devices.append(f"{name} - {size} - {model}")
        except Exception as e:
            self.log(f"Error listing devices: {e}")

        self.device_combo['values'] = devices if devices else ["No USB devices found"]
        if devices:
            self.device_combo.current(0)

    def run_cmd(self, cmd, check=True, capture=True):
        """Run a command and log output"""
        self.log(f"$ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
        try:
            if isinstance(cmd, str):
                result = subprocess.run(cmd, shell=True, capture_output=capture, text=True)
            else:
                result = subprocess.run(cmd, capture_output=capture, text=True)

            if result.stdout:
                self.log(result.stdout.strip())
            if result.stderr:
                self.log(result.stderr.strip())

            if check and result.returncode != 0:
                raise Exception(f"Command failed with code {result.returncode}")

            return result
        except Exception as e:
            self.log(f"ERROR: {e}")
            if check:
                raise
            return None

    def run_sudo(self, cmd, check=True):
        """Run command with sudo using stored password"""
        self.log(f"$ sudo {' '.join(cmd) if isinstance(cmd, list) else cmd}")
        try:
            if isinstance(cmd, list):
                full_cmd = ['sudo', '-S'] + cmd
            else:
                full_cmd = f"sudo -S {cmd}"

            if isinstance(full_cmd, str):
                result = subprocess.run(
                    full_cmd,
                    shell=True,
                    input=self.sudo_password + '\n',
                    capture_output=True,
                    text=True
                )
            else:
                result = subprocess.run(
                    full_cmd,
                    input=self.sudo_password + '\n',
                    capture_output=True,
                    text=True
                )

            if result.stdout:
                self.log(result.stdout.strip())
            if result.stderr:
                # Filter out the password prompt from stderr
                stderr = result.stderr.replace('[sudo] password for doug:', '').strip()
                if stderr:
                    self.log(stderr)

            if check and result.returncode != 0:
                raise Exception(f"Command failed with code {result.returncode}")

            return result
        except Exception as e:
            self.log(f"ERROR: {e}")
            if check:
                raise
            return None

    def start_build(self):
        """Start the build process in a thread"""
        device_str = self.device_var.get()
        if not device_str or "No USB" in device_str:
            messagebox.showerror("Error", "Please select a USB device")
            return

        device = device_str.split()[0]  # Extract /dev/sdX

        # Confirm
        result = messagebox.askyesno("Confirm",
            f"This will COMPLETELY ERASE:\n\n{device_str}\n\nAre you absolutely sure?")
        if not result:
            return

        # Double confirm
        result = messagebox.askyesno("Final Warning",
            f"FINAL WARNING!\n\nAll data on {device} will be permanently destroyed.\n\nContinue?")
        if not result:
            return

        self.building = True
        self.build_btn.config(state=tk.DISABLED)
        self.cancel_btn.config(state=tk.NORMAL)
        self.log_text.delete(1.0, tk.END)

        # Run in thread
        thread = threading.Thread(target=self.do_build, args=(device,))
        thread.daemon = True
        thread.start()

    def do_build(self, device):
        """Actual build process"""
        try:
            source = self.source_var.get()

            # Verify source exists
            if not os.path.isdir(source):
                raise Exception(f"Source path not found: {source}")

            # Step 1: Unmount
            self.set_step(1, "Unmounting device...")
            self.run_sudo(f"umount {device}* 2>/dev/null || true", check=False)

            # Step 2: Create partition table
            self.set_step(2, "Creating partition table...")
            self.run_sudo(['parted', device, '--script', 'mklabel', 'gpt'])
            self.run_sudo(['parted', device, '--script', 'mkpart', 'ESP', 'fat32', '1MiB', '513MiB'])
            self.run_sudo(['parted', device, '--script', 'set', '1', 'esp', 'on'])
            self.run_sudo(['parted', device, '--script', 'mkpart', 'root', 'ext4', '513MiB', '20GiB'])

            # Step 3: Format partitions
            self.set_step(3, "Formatting partitions...")
            self.set_substep("Formatting EFI partition (FAT32)...")
            self.run_sudo(['mkfs.vfat', '-F32', f'{device}1'])
            self.set_substep("Formatting root partition (ext4)...")
            self.run_sudo(['mkfs.ext4', '-F', '-L', self.root_label, f'{device}2'])

            # Step 4: Verify label
            self.set_step(4, "Verifying partition label...")
            result = self.run_cmd(['blkid', f'{device}2'])
            if self.root_label not in result.stdout:
                raise Exception(f"Failed to set label {self.root_label}")
            self.log(f"✓ Label verified: {self.root_label}")

            # Step 5: Get EFI UUID
            self.set_step(5, "Getting EFI partition UUID...")
            result = self.run_cmd(['blkid', '-s', 'UUID', '-o', 'value', f'{device}1'])
            efi_uuid = result.stdout.strip()
            self.log(f"EFI UUID: {efi_uuid}")

            # Step 6: Mount partitions
            self.set_step(6, "Mounting partitions...")
            self.run_sudo(['mkdir', '-p', '/mnt/usb-root', '/mnt/usb-efi'])
            self.run_sudo(['mount', f'{device}2', '/mnt/usb-root'])
            self.run_sudo(['mount', f'{device}1', '/mnt/usb-efi'])

            # Step 7: Copy rootfs
            self.set_step(7, "Copying rootfs (this takes a while)...")
            self.set_substep("Copying files... please wait")
            self.run_sudo(f"rsync -aHAX --info=progress2 {source}/ /mnt/usb-root/")

            # Step 8: Copy EFI files
            self.set_step(8, "Copying EFI boot files...")
            self.run_sudo(f"rsync -av {source}/boot/efi/ /mnt/usb-efi/")

            # Step 9: Update fstab
            self.set_step(9, "Updating fstab with EFI UUID...")
            self.run_sudo(f"sed -i 's/UUID=EFI_UUID_PLACEHOLDER/UUID={efi_uuid}/' /mnt/usb-root/etc/fstab")
            self.log("Updated fstab:")
            self.run_cmd(['cat', '/mnt/usb-root/etc/fstab'])

            # Step 10: Sync and unmount
            self.set_step(10, "Syncing and unmounting...")
            self.set_substep("Syncing buffers to disk...")
            self.run_cmd(['sync'])
            self.run_sudo(['umount', '/mnt/usb-efi', '/mnt/usb-root'])

            # Done!
            self.progress['value'] = 100
            self.step_label.config(text="✓ BUILD COMPLETE!")
            self.substep_label.config(text="")
            self.log("\n" + "="*50)
            self.log("SUCCESS! USB is ready to boot!")
            self.log("="*50)

            # Show partition info
            self.run_cmd(['blkid', f'{device}1', f'{device}2'])

            self.root.after(0, lambda: messagebox.showinfo("Success",
                "USB build complete!\n\nThe USB drive is ready to boot."))

        except Exception as e:
            self.log(f"\n!!! BUILD FAILED !!!\n{e}")
            self.step_label.config(text="✗ BUILD FAILED")
            self.root.after(0, lambda: messagebox.showerror("Build Failed", str(e)))

            # Try to unmount on failure
            try:
                self.run_sudo("umount /mnt/usb-efi /mnt/usb-root 2>/dev/null || true", check=False)
            except:
                pass

        finally:
            self.building = False
            self.root.after(0, lambda: self.build_btn.config(state=tk.NORMAL))
            self.root.after(0, lambda: self.cancel_btn.config(state=tk.DISABLED))

    def cancel_build(self):
        """Cancel is not really implemented - just a placeholder"""
        messagebox.showwarning("Warning",
            "Cannot safely cancel mid-build.\nPlease wait for current step to complete.")


def main():
    root = tk.Tk()
    app = USBBuilder(root)
    root.mainloop()


if __name__ == "__main__":
    main()
