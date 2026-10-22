# Using Storage with a NAS

Storage works with any NAS (or Linux/BSD server) you can reach over **SSH**: Synology, QNAP,
TrueNAS, Unraid, OpenMediaVault, a Raspberry Pi… It needs nothing installed on the NAS and no
extra software on your computer, because it uses the `ssh` command that already comes with
macOS, Linux and Windows 10/11.

## 1. Enable SSH and key login (once)

1. Turn on SSH in your NAS settings (e.g. Synology: *Control Panel → Terminal & SNMP → Enable SSH*).
2. On your computer, create a key if you don't have one, then copy it to the NAS:

   ```sh
   ssh-keygen -t ed25519            # press Enter to accept the defaults
   ssh-copy-id admin@192.168.1.20   # Windows: type %USERPROFILE%\.ssh\id_ed25519.pub | ssh admin@192.168.1.20 "cat >> .ssh/authorized_keys"
   ```

3. Check it works: `ssh admin@192.168.1.20 echo ok` should print `ok` without asking for a password.

Storage never stores passwords. It uses your keys and ssh-agent, and it respects
`~/.ssh/config`, so a host alias like `Host nas` works as the host name. The first connection
accepts the NAS's host key and records it in `known_hosts`; after that a changed key is refused.

## 2. Add the NAS

**App:** in the sidebar, under *Network*, click **Add NAS…**. Enter the host, user and the folder
to scan (e.g. `/volume1` on Synology, `/mnt/user` on Unraid, `/mnt/<pool>` on TrueNAS), then
click **Test Connection**.

**CLI:**

```sh
storagectl nas add home --host 192.168.1.20 --user admin --root /volume1
storagectl nas test home
```

## 3. Use it like a local drive

Everything works on the NAS: the size browser, duplicates, organizing, insights, history and undo.

```sh
storagectl --nas home scan .                       # the NAS's default folder
storagectl --nas home du photos -d 2               # relative paths are inside the default folder
storagectl --nas home dupes /volume1/photos --min-size 1M
storagectl --nas home organize /volume1/inbox --preset photos --apply
storagectl undo 12                                 # reconnects to the right NAS automatically
```

## How it works

- **One SSH connection per worker.** Storage keeps up to four shell sessions open and sends
  commands down them. Each command's output ends with a random marker, so there's no per-command
  login delay.
- **The work happens on the NAS.**
  - *Scanning* is one `find` command that streams back the whole folder tree.
  - *Hashing* runs `sha256sum` on the NAS (partial hashes via `head`/`tail`), so file contents
    never cross the network.
  - *Moves* are `ln` + `rm` (atomic, and they refuse to overwrite) or `mv` for folders.
  - *Copies between volumes* are verified with a fresh hash before the original is removed.
- **Tool detection.** GNU `find -printf` is used when available. Otherwise Storage falls back to
  BusyBox/GNU `stat` or BSD `stat` (TrueNAS CORE), and the right hashing tool is picked
  automatically (`sha256sum`, `sha256` or `shasum`).
- **"Trash" on a NAS** is a `.storage-trash` folder at the top of the same volume (the highest
  folder on that filesystem you can write to). Moving into it is a rename, so it's instant and
  can be undone. Empty it yourself when you're sure.
- **Snapshots are skipped.** ZFS `.zfs` and Synology `#snapshot` folders would double-count
  everything, so scans skip them.
- **Same journal as local drives.** Every change is recorded before it happens. If the
  connection drops, the interrupted step is checked and finished the next time Storage reaches
  the NAS.
