# Organize rules

A **rule set** decides where every file in a folder should go. You can build one in the app's
Organize view (Ctrl/⌘+3), or write it as JSON and use it with
`storagectl organize <folder> --rules file.json`.

## How a rule set is applied

1. Each file is checked against the rules **in order**. The **first** rule that matches decides
   what happens to it.
2. A matching rule either moves the file into a **folder template** (and optionally renames it),
   or **leaves** it where it is.
3. Files that no rule matches go to `fallbackFolder` ("Everything else"). If that is empty, they
   are left alone.
4. The result is a **preview**. Nothing moves until you confirm, and every move can be undone from
   History.

## Templates

Folder templates use `/` between levels, e.g. `Photos/{year}/{year}-{month}`. Tokens:

| Token | Example | Meaning |
|---|---|---|
| `{type}` | `Images` | Kind of file: Images, Videos, Audio, Documents, Archives, Code, Applications, System, Other |
| `{ext}` / `{EXT}` | `pdf` / `PDF` | Extension (`no-extension` if none) |
| `{name}` | `IMG_0042` | File name without extension |
| `{filename}` | `IMG_0042.jpg` | Full file name |
| `{year}` `{month}` `{day}` | `2024` `07` `14` | Photos use the EXIF *date taken*; everything else uses the modified date |
| `{monthname}` | `July` | Month name |
| `{size}` | `Medium (1-100 MB)` | Small / Medium / Large / Huge |
| `{first}` | `I` | First letter (`0-9` or `#` for digits and symbols) |
| `{parent}` | `Camera Roll` | Name of the folder the file is in now |

A **rename** template works the same way, e.g. `{year}-{month}-{day} {name}`. The original
extension is kept unless the template adds one. Characters that aren't allowed in file names on
every OS (`/ \ : * ? " < > |`) are replaced with `_`.

## JSON format

```json
{
  "name": "My inbox",
  "description": "Invoices by year, big videos aside, everything else by type",
  "rules": [
    {
      "name": "Invoices",
      "match": { "types": ["Documents"], "nameContains": "invoice" },
      "folder": "Finance/Invoices/{year}",
      "rename": "{year}-{month}-{day} {name}"
    },
    {
      "name": "Big videos",
      "match": { "types": ["Videos"], "minSize": 1073741824 },
      "folder": "Videos/Large"
    },
    {
      "name": "Keep screenshots here",
      "match": { "nameRegex": "^Screen ?Shot" },
      "leave": true
    }
  ],
  "fallbackFolder": "{type}",
  "dateSource": "capture",
  "conflict": "rename",
  "includeSubfolders": true,
  "removeEmptyFolders": true,
  "includeHidden": false
}
```

| Field | Values |
|---|---|
| `match.types` | Any of the kinds listed under `{type}` |
| `match.extensions` | `["pdf", "docx"]`, written without dots |
| `match.nameContains` | Case-insensitive text |
| `match.nameRegex` | ECMAScript regular expression, case-insensitive |
| `match.minSize` / `maxSize` | Bytes |
| `match.olderThanDays` / `newerThanDays` | By modified date |
| `dateSource` | `capture` (EXIF date taken, else modified) or `modified` |
| `conflict` | `rename` (keep both: `name (2).ext`) or `skip` |

Start from a built-in preset: `storagectl presets --show "Photos & videos by date" > mine.json`.

## What is never moved

- Anything inside app bundles, photo/music/video libraries, Xcode projects, `.git`,
  `node_modules`, virtual machines and similar packages. These are treated as single opaque items.
- Hidden files and folders, unless `includeHidden` is on.
- OS junk files (`.DS_Store`, `Thumbs.db`, `desktop.ini`). They are deleted only when the folder
  holding them is otherwise empty and is being removed.
