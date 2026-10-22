# Craft Missing Parts

Choose an item to craft or something to build, and this mod crafts the parts you are missing.
If a part's own parts are missing too, it goes back and crafts those as far as it can.

**Needs the new Lua functions from PR #10477.** The dev branch of this fork has them. On a game without them, the menu only shows a notice and does nothing.

## How to use

1. Action menu → "Craft missing parts"
2. Choose "Choose an item to craft" or "Choose something to build", type part of the name and pick the target
3. The missing parts are listed with whether each can be made; answer "Yes" to start

- **Crafting**: when all parts are ready, the target item is crafted too.
- **Construction**: you are told when the materials are ready. Build it from the construction menu.
- Open the menu again while it runs to choose "Stop".

## How it works

- After each part, it re-checks **what you carry and items within 6 tiles** to decide the next part (same range as normal crafting).
- Crafting is the same as from the crafting menu: skills, failure and time are the same. Answer any prompts, such as which components to use.
- When there are several ways to make a part, one that can be made right now is preferred.
- Only **recipes you know** are used.

## Notes

- Missing tool qualities (cutting, hammering, ...) are not crafted. The needed qualities are shown on the confirmation screen and when it stops.
- It stops if crafting is interrupted or fails. Finish any unfinished item, then choose "Continue" in the menu.
- To avoid unexpected loops, it stops after crafting parts 50 times, or the same part 20 times.

## Settings

Change them in `CFG` at the top of `main.lua`.

- `max_depth`: how many levels of parts-of-parts to search
- `max_crafts`: max number of parts crafted in one run
- `max_same`: max number of times the same part is crafted

## Credits

Author: sagittarius72. Made with the help of Claude (Anthropic).
