# Automation: CLI, REST API, MCP (Claude) and Ollama

Storage can be driven by scripts and AI agents in four ways. All of them share one rule:
**anything that changes files is two steps.** A planning tool returns a summary and a `plan_id`,
and nothing happens until `apply_plan` is called with `"confirm": true`. Every applied change is
journaled and can be undone.

## 1. Command line (`storagectl --json`)

Every command prints machine-readable JSON with `--json`, so an agent working over SSH or in a
terminal can call it directly:

```sh
storagectl --json du ~/Projects -d 2 -n 10
storagectl --json dupes ~/Pictures --min-size 1M            # dry run: groups and what would be kept
storagectl --json organize ~/Downloads --preset tidy        # dry run: every planned move
storagectl dupes ~/Pictures --apply --yes                   # act (journaled)
storagectl --json history && storagectl undo 7 --yes
storagectl --nas home --json insights /volume1/photos       # same commands on a NAS
```

## 2. MCP server for Claude (`storagectl mcp`)

[MCP](https://modelcontextprotocol.io) lets Claude use Storage as a set of tools.

**Claude Code:**

```sh
claude mcp add storage -- storagectl mcp
# macOS app bundle: claude mcp add storage -- /Applications/Storage.app/Contents/MacOS/storagectl mcp
```

**Claude Desktop** (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "storage": {
      "command": "/Applications/Storage.app/Contents/MacOS/storagectl",
      "args": ["mcp"]
    }
  }
}
```

Then ask things like *"What's using space in my home folder?"*, *"Find duplicate photos on my
NAS and tell me how much I'd save"* or *"Tidy my Downloads folder"*. Claude will show you the
plan, and it only calls `apply_plan` after you agree. Tools that change files are marked
destructive in the tool annotations, so MCP clients can ask for confirmation too.

### Tools

| Tool | Changes files | What it does |
|---|---|---|
| `scan` | no | Scan a folder (local or `nas`) and save it to the index |
| `du` | no | Size tree of a folder (`depth`, `top`) |
| `list_scans` | no | Saved scans |
| `find_duplicates` | plans | Duplicate groups + `plan_id` (`keep`, `action`: trash/hardlink/clone/delete, `min_size`) |
| `plan_organize` | plans | Moves for a `preset` or custom `rules` (+ `destination`) + `plan_id` |
| `plan_cleanup` | plans | Cache/junk sizes on this computer + `plan_id` (`targets`, `permanent`) |
| `insights` | no | Large old files, idle `node_modules`/build folders, empty folders |
| `apply_plan` | **yes** | Run a plan once; requires `confirm: true` |
| `history` | no | Recent batches |
| `undo` | **yes** | Revert a batch; requires `confirm: true` |
| `presets` | no | Built-in organize rule sets |
| `nas_list` | no | Saved NAS connections |

Every tool that takes a `path` also takes an optional `nas` (the name of a saved NAS).

## 3. REST API (`storagectl serve`)

```sh
storagectl serve                      # http://127.0.0.1:8765, token saved in the app data folder
```

The server only listens on `127.0.0.1` unless you pass `--host`. Everything except
`/v1/health` needs `Authorization: Bearer <token>`. The token is printed at startup and stored
in `api-token` in the app data folder (readable only by you).

```sh
TOKEN=$(cat ~/Library/Application\ Support/Storage/api-token)   # Linux: ~/.local/share/storage/api-token
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/v1/tools
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://127.0.0.1:8765/v1/tools/find_duplicates \
     -d '{"path": "/Users/me/Pictures", "min_size": 1048576}'
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://127.0.0.1:8765/v1/tools/apply_plan \
     -d '{"plan_id": "plan-1", "confirm": true}'
```

| Method | Path | |
|---|---|---|
| GET | `/v1/health` | `{"ok": true, "version": …}`, no token needed |
| GET | `/v1/tools` | Tool list with JSON schemas (same as MCP) |
| POST | `/v1/tools/<name>` | Body = tool arguments; response = tool result, `400` for bad arguments |

## 4. Ollama: AI-suggested organize rules

With [Ollama](https://ollama.com) running locally (`ollama pull llama3.2`):

- **App:** in Organize, click **Suggest with AI…**, pick a model and optionally describe what you
  want. The suggested rules appear as a new rule set and are previewed like any other.
- **CLI:** `storagectl organize ~/Inbox --ai --goal "invoices by year, photos by month"`

**What is sent:** only a summary of the folder, meaning counts per kind of file and extension,
a sample of up to 150 file names, the date range and existing subfolder names. **File contents
are never read or sent.** Ollama runs on your machine by default; point `--ollama` (or the app
setting) at another host only if you trust it.

The model's answer must match a JSON schema and is validated exactly like hand-written rules. If
it is invalid, the model gets one chance to correct it. Nothing moves until you review the
preview and click Organize.
