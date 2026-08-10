Difficulty: E/N/H

# Step 1: get out of 0 marks as soon as possible
Urgent task:
* N Ensure echo server works: acceptor + playerio + auth
* E Make tetrisu a REPL: parse inputs into arrays of command line args: example:
```
foo bar 'baz \r' "bah'\" \n" 
```
should be turned into
```
["foo", "bar", "baz \\r", "bah'\" \n"]
```
Just 1 function.
`'quote'` means content inside is not escaped. `"quote"` means content inside is escaped.
* E Add HTTTP layers into sever
* E Make a command `htttp arg` in client that can send a request to server.
* N Add application layer with player management, including metatdata of at least name.
* E Make a command `set-name name` and `whoami` in client to query and post names (and also id). Keep id as fd for simplicity.
* N Re-add `tetrislogd` into the current system
* H Set up a singleplayer mode. 
* H Integrate to client side. Command: `singleplayer`. Port the rendering from `~/folder/tetris`.
* N Set up `tetrisctl`.
* N Clean up the mess from `tetrish`. `tetrish` barely works and have footguns like UB if arg > 64. In fact let `tetrish` have the cmdline system of `tetrisu` too.

Note: also show the line clear tracker in tetrisbrain to conform w specs.
**CHECKPOINT**: 

# Step 2: battle royale
* H Set up room system (no game state yet)
* N Add commands `create`, `join [room-id]`, `leave`. No need to support host: everyone has full privilege.
* N first broadcast attempt: command `start` sends everyone a string, can be redone
* N persistent stage: temporary command `dbg-set-string [string]` string, `dbg-get-string` to be able to mutate state `dbg-quit` to end room (requires `start` again)
* H timer: hides `dbg-get-string`, server push `STATE` every seconds
* N input: replace `dbg-set-string` with move system: `MOVE`/`ROTATE`/`DROP`. `dbg-get-string` is now each player's input.
* N game: integrate `tetrisbrain`, using local system to send garbage.
* H rendering & menu: Use the proper ui system defined in singleplayer. game now must be able to separate screen & input place
* H garbage queue ipc

* **CHECKPOINT**: At this point, battle-royale hard part is done. Must be able to create room, tell your friends about room code & play. 

# Step 3: improvement 1
* N enforce host on operations & host transferring
* **CHECKPOINT**: At this point, everything is covered in spec.

* H config system: room visibility, public room list, query public room list with `get-public-rooms`/`next-public-rooms`/`previous-public-rooms` that boils down to get room from idx `[internally-tracked*20, (internally-tracked + 1)*20)`. Accept extra arguments in `create` for configuration (e.g. `create --public`).

* **CHECKPOINT**: At this point, game is sane to showcase.
* N scaling in game + configuration to set game info.
* E (optional): `chat --player id msg`, `chat msg` to chat in rooms. Needs better UI however.

# Step 4: winning prize (we even have time to do this? very hard)
* N configuration to play as 4-wide.
* H puyo fever mode but in tetris: cancelling garbage gains stack. on 7 stack enter fever mode where garbage behind is frozen, enter 4-wide for some time &. **This one is literally a tetrisbrain rewrite**
* H better UI 

Document + test must be done on every checkpoint or when it feels like new features are not worth it anymore.
