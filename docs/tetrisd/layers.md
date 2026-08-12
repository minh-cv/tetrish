# Domain layers of `tetrisd`
## Overview
Each major task in `tetrisd` (e.g. IO, authentication, HTTTP parsing/serialization) is handled in a layer. Layers have processing functions that receive inputs of type `{Fd: [Object1]}`, and return result(s) of type `{Fd: [Object2]}` plus, optionally, a set of error entries `{Fd}`. The returned result can be chained into another layer. A processing function may also read and update the layer's persistent per-key state (e.g. the auth state machine), so the full shape is `F(state, {Fd: [In]}) -> ({Fd: [Out]}, {Fd_err})`.

> The above input/output type is preferred over `(Fd, [Object])` or `(Fd, Object)` to discourage external sequencing between unit operations on `(Fd, Object)`: you should only do parsing once you finish decryption, rather than interleaving between them.

> Internal sequencing between unit operations on `(Fd, Object)` is allowed: if a unit operation fails, the rest of `Object` is not processed, and the main output won't include `Fd` as a key.

Every tick chains the layers in a fixed order: poll, accept fan-out, read, handshake/decrypt, (application), encrypt, write, interest sync, close fan-out, then per-layer resets. Writing is driven by readiness only: frames produced in a tick wait in the write state until the poller reports the socket writable.

These conventions apply to layers that own several categories of per-fd state chained between neighbors. Single-purpose components (e.g. the acceptor, the logger) do not carry this structure and only follow the lifecycle naming where it makes sense.

## Data structure

### Sparse set
The equivalent of a map `{size_t: U}` is represented as a sparse set. Internally, the sparse set is a sparse array of slots indexed by element's key, plus a dense collection (here we use an array) containing the active elements. Each slot contains the state of the slot, the iterator at the active element (if exists), and memory for emplacing object of type `U`.

If the sparse set's lifetime is active, a slot can change between three states:
* Uninitialized/freed: `U`'s lifetime has ended or has not started.
* Inactive: `U`'s lifetime is active but cannot be found in the active list.
* Active: `U`'s lifetime is active and can be found in the active list.

> Inactive exists to prevent repeated allocation where `U` is a collection. Ideally, we could move from active to freed and destroy `U`, but that means every transition between freed and active reallocates and frees memory. Read more about the inactive state in the layer API.

> Alternatively, one could consider why freed is not represented as inactive, and actually free the slot only when the sparse set is freed. It is possible, but it requires resizing operations (which this codebase avoids), and the three states express stricter invariants.

The sparse set supports `O(1)` insertion, `O(1)` deletion, `O(1)` access and `O(1)` traversal between active slots.

### Ring buffer
Originally used as a queue for `Reader`/`Writer`, ring buffers are recycled as a random access collection. They provide `O(1)` front and back insertion, front and back removal, and random access. Object addresses are stable.

## Layer API
### Overview
Each layer's processing functions (minus the topmost one) receive input of the previous layer and output the input type of the next processing operation, plus extra information for other operations. Each layer is also associated with a type that contains
* Persistent state between ticks necessary for processing functions (e.g. session key is persistent, because once you received it, you need to store it to decrypt and encrypt).
* Input/output states that serve as input/output parameters of its processing functions or its neighbor's processing functions. These states are consumed on the same tick and removed at the end of the tick. For example, a parsed HTTTP frame is consumed immediately by the Application layer.

#### Data members
The type of the layer usually has
* A mapping of the key type (usually, integer type representing a file descriptor) to the associated persistent state. These are implemented using `SparseSet` with type named as `SparseSet_[LayerType]Entry` (e.g. `SparseSet_AuthEntry`) and member simply named as `entries`. Membership in `entries` is the layer's definition of which keys exist: a key is in `entries` iff its per-entry state (and its slots in the layer's other collections) is initialized.
* Collections mapping from key type to a particular category of input/output states. These are normally implemented using `SparseSet` with type named as `SparseSet_[StateType]`. Very often, the state type is a queue (representing the `[Object]` in `{Fd: [Object]}`), so a mapping from `Fd` to a queue of `Frame` `FrameQueue` would be named as `SparseSet_FrameQueue`. A plain key set `{Fd}` (e.g. the error set) is `SparseSet_bool`.

#### Member functions
Convention: if an operation reads/writes on input/output state, make a parameter named `m_[member_name]` where `[member_name]` is the name of the member containing such state. It stays an explicit parameter so the call site shows the dataflow, and so a state could move between layers without a signature change.

* Initialization `[LayerType]_init()`: called once on server setup, receives parameters to allocate whole-collection memory and store already-initialized persistent state (e.g. credentials). Per-entry state is not allocated here.
* Accept `[LayerType]_accept()`: called on a list of key objects to initialize entries for them, receives parameters to allocate memory to each entry. Erroneous key objects are marked in the error set.
* Process `[LayerType]_[process_operation_name]()`: called on every tick.
* Reset `[LayerType]_reset()`: called at the end of every tick, returns per-tick active slots to inactive.
* Close `[LayerType]_close()`: called on a list of key objects to remove those entries, freeing their per-entry state and deactivating any of their slots that persist across ticks.
* Free `[LayerType]_free()`: called when tearing down the server. Release all resources contained. Every entry must have been closed beforehand; shutdown drives all live keys through the close operations first.

> Since accept allocates per-entry state, a configuration reload applies its new sizes only to entries accepted afterwards; live entries keep the sizes they were accepted with.

> Reset is only legal when every active element has been fully consumed. Consumption is by reading in place, not by removal, so reset is the point where consumed data is reclaimed. Not every state is reset per tick: an output state that survives ticks (e.g. pending writes waiting for writability) keeps its slot active exactly while it is nonempty; the consuming operation deactivates the slot when it drains it, and close deactivates it otherwise.

### Ownership and consumption
* A processing operation consumes its input by reading in place, never by popping. Downstream layers may hold non-owning views into upstream buffers: a parsed HTTTP message points into the decrypted frame it was parsed from, so popping auth's decrypted queue would invalidate the application's input. Reclamation happens at reset (see above).
* Modifying the input in place is allowed but expected to be rare; the application layer is the only likely user.
* Writing is the exception: the write operation pops frames as they are fully flushed to the socket and frees them, since nothing downstream can hold a view into them.

### Error handling
* An operation's output contains exactly the keys whose work committed: on failure, the staged output for that key is discarded, its activation undone, and the key marked in the error set.
* Recoverable per-frame conditions (decrypt failure, oversized frame) are not operation failures. They travel in-band as error-status frames, and the last layer before the socket decides the close. Content is attached to a frame if and only if every status on it is OK; error branches never touch content.
* The error set doubles as the close set. Later operations skip keys already in it. For the persistent write state this skip is the stop-sending policy, and close reclaims whatever the state still holds.
* The forward (decrypted request) direction and the backward (encrypt input) direction are separate states with separate frame types, as their error variants diverge.

### Handshake output path
The handshake pushes its outbound frames directly into the write state. This is not a cross-layer mutation, because ownership of the slot is well defined: auth owns an fd's `write_qs` slot for the whole `handshake_or_decrypt` pass, and encrypt produces output for an fd only after that pass commits for it, so during the pass the slot contains handshake frames only. Ownership transfers to encrypt when the pass commits for the fd. A failed pass, whether the failure is in a handshake step or a decrypt step of the same tick, drains the slot and marks the error set, so the staged output is discarded per the commit-or-discard rule rather than lingering until close. In the transition tick (handshake completes and later frames of the same read batch are decrypted), encrypt runs only after the pass committed, so handshake frames always precede the first encrypted frame on the wire.

If a future feature ever needs to send to a not-yet-authenticated fd, this ownership argument breaks; the fallback is a separate handshake `{Fd: [out]}` output merged with encrypt's at write.