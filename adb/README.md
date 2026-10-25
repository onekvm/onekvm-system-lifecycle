# Temporary USB ADB

`onekvm-adb-debug.service` provides an explicitly requested local debug session,
including when Core or networking is unavailable. The initialization screen asks
for confirmation after the diagnostics dialog. No boot target or package hook
starts or enables this static service; a reboot ends the session.

For an authorized local shell:

```sh
systemctl start onekvm-adb-debug.service
systemctl stop onekvm-adb-debug.service
```

On the USB host, `adb devices -l` lists serial `onekvm`, product `OneKVM_Device`
and the detected model (for example `NanoKVM_PCIe` or `NanoKVM_Pro_Desk`).
`adb -s onekvm shell` opens a root Bash session directly, without sh compatibility
mode. The daemon package depends on Bash. USB product text is
`OneKVM Device - <model>` for the session; disabling debugging restores the
original serial and product unless Core or the user has since changed them.
Only USB transport is used. There is no TCP listener or automatic upload.
Enabling and disabling reconnects USB, interrupting HID/audio/media briefly.
The active Core watchdog is paused for the debug session and resumed on cleanup.
An already inactive watchdog stays inactive.

The helper and Core serialize gadget changes with
`/run/onekvm-usb-gadget.lock`. The helper adds FunctionFS through `zz_adb` while
preserving existing functions. If no gadget exists it creates a minimal gadget;
Core may start and add its functions during the session. Cleanup removes only
the debug function and restores the remaining composite gadget.

The service reports readiness after endpoints are available and the UDC is bound.
Runtime ownership markers under `/run/onekvm-adb-debug` let `ExecStopPost` recover
after a forced wrapper exit. Normal shutdown detaches ADB while ep0 is alive,
then terminates adbd and unmounts FunctionFS. Failed starts are visible in the
systemd journal and the screen's feedback dialog.

The `onekvm-adbd` package contains only the daemon from android-tools; the stock
Android gadget/autostart packages must not be installed for this feature.
