# Lore of `tetrisd`
## Step 1: [IO](https://github.com/minh-cv/tetrish/commit/a3e67749e484a349886d05cfa32f33ad6cfb1efe)
An `epoll` server requires reading/writing to be able to give up control at any time due to blocking. Our first vision of `tetrisd` is that each connection is encapsulated into a state machine called `struct client`. This state machine, in addition of reading/writing, can also store additional state and performing operations after performing a request of reading `n` frames.

The state machine can be summarized like this:
* The initial state is (read 1 frame, nonce state)
* After reading operation:
  * If auth state is nonce: write 2 frames of certificate & signed nonce
  * If auth state is symkey: read 1 frame, skipping write
  * If auth state is done: write 1 frame of dummy response
* After writing operation: read 1 frame (because at any auth state, the number of frames read is still 1)

## Step 2: [Domain logic](https://github.com/minh-cv/tetrish/commit/4b45bb916d20055555618004e4018ad4eabe75b9)
*July 10*

Our `epoll` server now needs to serve different connection types: other than clients, it needs to be able to distinguish a logger fd, and in the future, more kinds of connection like a control panel. We want to share this reading/writing capability, but first, we need to determine the common structure first. If you strip away the client-specific part, it could be considered a template like this

* The initial state is some per-type `io state, domain state`, where `io state` is one of `read x frames` or `write x frames`.
* After performing IO state, transition the state as `old io, old domain state -> new io, new domain state`.

Doesn't this look like inheritance? We could define a base class to perform I/O logic, then override the state transition. With that, we are able to create a logger client whose behavior is to only to write frames, completely different from our normal player client.

With some upcasting, the information of a `ClientUnauthed` can be replaced with `TetrisClient`. This allows modularity of authentication as a client type, and can be reused in other projects.

## Step 3: [Logger](https://github.com/minh-cv/tetrish/commit/2fd35bcc93aef8aa92f40f17c560faf20ba215ba)
*July 15*

What should we do if the logger buffer queue is empty? Just disarm `EPOLLOUT` and disable logging.

Issue: our logger has no access to epoll to do so. Trying to do so means that it can control epoll, which is crazy in terms of responsibility. Fortunately, `CLIENT_IO_YIELD` is designed exactly for this: return control to the caller and let caller handle it.

Next issue: once `EPOLLOUT` is disarmed, `epoll_wait` never polls it again, so logging becomes dead forever. The solution is simple, just drive the state machine again once there are logs produced. 

I went for a more general solution which allows arming once there is a state transition to write. It works splendidly. Our networking system is now expended to
```
        poll ------+-> resume_client_event  -----> WOULDBLOCK: return
                   |                ^       -----> YIELD: dispatch to handler
                   |                |       -----> ERROR: close connection
                   |                |       -----> CONTINUE
                   |                +-----------------+
                   |
outside trigger ---+
(e.g. log buffer size > 0)
```

## Step 4: Room ticks

Let's replace our dummy `200 OK`-responding server to allow playing game. We just need to wire `tetrisbrain`, then tick for 60Hz, advancing the game board for all players.

What happens when a tick is reported by poll? We just find the client, handle the game board, then write a `STATE` frame and transit the IO state to write... until you realize you can't, because the client could be in the middle of reading/writing when you do so, and forcing it to write can corrupt the state.

I remember very well some of the insane "solutions" formed. One of them was viewing the polling action in `main` and timer ticking as 2 threads, and the act of timer trying to force client transit to write as accessing the critical section. Derived a software version of Peterson's algorithm. Funnily, this is a creatively wrong solution: there is nothing to repeatedly check to enter the CS, and modelling them as a thread is wrong.

Any attempt to change the networking system feels like violating all sort of encapsulation. Upon reflection, I realized the assumption `ClientIo` was built on contradicts the operation we required. I love to view the response pushed to the client as a function `request, domain state -> response`. The shattering reality is that now `response` can be produced by a completely different operation.

> At the time of writing this line, I find out a much simpler solution: storing the STATE and the information of having STATE after transition (in `CLIENT_READ_TRANSIT`) to push the frame to write. What the rationale of this solution is is to reframe the "different operation" as a mutation of `domain state`.

Not only that, the model is full of baggage.
* The control flow is too complex: `CLIENT_IO_YIELD` can allow a client operation to do anything, and .
* Bad separation of IO logic and domain logic: everything is tied under the callback called once reading/writing is done. That function, in addition of domain logic (parsing/serializing, process the input to make the response), must drive the state machine.
* Horrible accessibility: to not break encapsulation, `ClientIo` assume that the domain state is all needed to form a response. In reality that is not true: not only you need to read states from elsewhere (e.g. querying room list), but also write to them. Most of the operations then must be done through `CLIENT_IO_YIELD`, and it puts a lot of stress on the server because in addition to processing that request, the server must resume that clunky state machine.

With those massive issues, nuking and redesigning the server is the only option left: we barely touched domain logic and the system is already showing signs of inflexiblility. In fact, I wonder how I can tolerate such an overly complex system for that long.

## Step 5: [Association](https://github.com/minh-cv/tetrish/commit/4f3f1d2d1a7287a2991068138bcc4a8938ebf126)

*August 1, 2 weeks before the deadline*

Our rewritten `200 OK` server now replaces the polymorphism-based `ClientIo` and unroll all of the logic. Control flow is now more restricted and predictable: `tick` -> dispatch to specific handler, usually an IO wrapper of entry to domain logic. Coupling caused by IO is gone: the boundary layer is only a wrapper plus some extra logic to handle closing.

With the first 2 issues gone, accessibility is the real bane left. It can be summarized like this: how do you handle accessing and performing operations on  data outside of a client's state? Narrowing it down gives: how do you represent associations between sibling types (types whose relationship does not fall under aggregation/composition), like "a room has multiple seat, each could be associated to 1 client"?

I have two answers. 
* The first is storing the information to access them directly in the data of the accessor, which I will use the term **reference**. Example: store a pointer of their current room in client's data. The term reference here is more generic than a pointer: indices, generational handles, access to the container of the data also counts.
* The second is to do **dependency injection**, where you make the caller pass enough information so that the leaf functions can handle everything.

I decided to follow the first. However, another question arises: whether the reference itself is valid when accessing. Say, after a client IO operation failed, the server closes the client. If the room store the association of the client as its fd, that data can be outdated, and the room can act on false data. These kinds of mistake can very easily lead to UB (e.g. OOB read/write). Worse, there is no way to know whether that happens if you use primitives like pointer and index.

After researching, I invented my own solution. Inspired by C++'s `weak_ptr` ability to resolve to `nullptr` if the `shared_ptr` is gone, I make a similar reference-counting system, `Referent` and `Ref` (reference). 
* `Referent` either contains a value or not. While containing a value, `Ref` can be created to reference it.
* When a new value is constructed in `Referent`, the `Referent`  must not be referenced by any `Ref` at that point.
* There are 2 destroy semantics on `Referent`
  * `destroy`, which is that `Ref` resolves to `NULL`. These `Ref`s must be destroyed before a new value is constructed per previous rule.
  * `consume`, which is `destroy` following with assertion that no `Ref` is referencing it.

This solution is also aimed at the second problem: handling rippling effects (e.g. closing a client announces to everyone about it. The "closing" can be detected if the pointer resolves to nothing.)

However, something is still off. `consume` semantics requires very strong invariant and can break single responsibility to handle it. For example, if the connection manager want to `consume` the client due to client IO error, suddenly it needs to call the domain logic of what needs to be ripped off. If the client is in a room, you must rip off the `Ref` in that room and prepare to broadcast about that. These logics are nontrivial, so the act of closing a connection now can trigger a very complex chain of sequences.

`destroy` is not much better. Even though it allows deferring of such event, it lacks an answer of natural entry point to resolve such events. Unless explicit tracking is done, `Ref` will only resolve to `NULL` in the middle of other domain logic. This leads to the precondition of such operations becoming complex, and handling of edge conditions being scattered.

A side consequence in C is that `Referent`/`Ref` are implemented using X-macros, and these can become so verbose which severely hurts ergonomics. 

Would dependency injection solve these issues? No, because it still didn't answer how to deal with the lifecycle issue. The call of closing happens outside the time where data is passed to domain logic, so it is irrelevant to the issue. In fact, it probably would make it worse because there is no way to track the relationship outside of the duration of the call.

My conclusion is that while `Referent`/`Ref` provides safe invalidation, they provide too little insights on how to handle the semantic consequences of destruction. In the end, `Referent`/`Ref` never saw the light of day; it got scrapped along with the entire current architecture. 

## Step 6: [Centralization](https://github.com/minh-cv/tetrish/commit/9685e7a0e6e740e5bd43b3d35b1e1d235512b91d)
*3 days before the deadline*

During the period of the first redesign, I got a question to myself. If we consider the sequence of operations on a client like `read -> decrypt -> parse -> process -> serailize -> encrypt -> write`, how would I like to structure the main loop of the server?

Option 1:
```
for each client {
  read
  decrypt
  parse
  process
  serialize
  encrypt
  write
}
```

Option 2:
```
for each client { read }
for each client { decrypt }
for each client { parse }
for each client { process }
for each client { serialize }
for each client { encrypt }
for each client { write }
```

