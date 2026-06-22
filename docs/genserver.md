<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# GenServer: Technical Reference

Standardized actor patterns for Trix, inspired by Erlang/OTP gen_server.

---

## 1. What GenServer Is

A GenServer is an actor running a standardized receive loop that dispatches
incoming messages to user-defined handler procs.  It provides three
communication patterns out of the box:

| Pattern  | Operator           | Semantics                                             |
| -------- | ------------------ | ----------------------------------------------------- |
| **Call** | `gen-call`         | Synchronous request/reply (caller blocks until reply) |
| **Cast** | `gen-cast`         | Asynchronous fire-and-forget                          |
| **Info** | (raw `actor-send`) | Non-protocol messages (passthrough)                   |

```
% Define a counter server
<<
  /init { 0 }
  /handle-call {
    [/msg /from /state] let
      state 1 add dup dup /reply
    end
  }
>> gen-server /counter exch def

% Call it (synchronous).  gen-call blocks on a reply mailbox, so the
% caller must itself be an actor -- the main coroutine has no mailbox.
mark {
  counter /inc gen-call    % => 1
  counter /inc gen-call    % => 2
  counter /inc gen-call    % => 3

  % Shut it down
  counter /normal gen-stop
} actor-spawn coroutine-join pop
```

GenServer encodes decades of Erlang wisdom into a reusable pattern:
state threading, message categorization, reply delivery, and graceful
shutdown -- all handled by the runtime.

### 1.1 Why GenServer Exists

Trix already has actors (`actor-spawn`, `actor-send`, `actor-recv`).
But building a well-behaved server from raw primitives requires solving
the same problems every time:

1. **State threading** -- receive message, compute new state, loop.
   Easy to forget the loop or mismanage the state variable.
2. **Request/reply** -- correlate replies to requests.  Requires unique
   refs, mailbox scanning, and careful message formatting.
3. **Graceful shutdown** -- drain pending work, run cleanup, die.
4. **Message categorization** -- distinguish protocol messages (calls,
   casts) from raw messages (info).

GenServer solves all four.  The user writes handler procs (pure business
logic); the runtime handles the machinery.

### 1.2 GenServer vs Raw Actors

| Aspect           | Raw Actors                         | GenServer                             |
| ---------------- | ---------------------------------- | ------------------------------------- |
| State management | Manual (variable in recv loop)     | Automatic (threaded through handlers) |
| Request/reply    | Manual (build refs, scan mailbox)  | `gen-call` / `/reply`                 |
| Message format   | User-defined                       | Internal protocol (transparent)       |
| Shutdown         | Manual (send poison pill, cleanup) | `gen-stop` + optional `/terminate`    |
| Boilerplate      | ~20 lines per server               | 5 lines (spec dict)                   |

### 1.3 Relationship to Other Systems

```
GenServer
  |
  +-- uses Actors (actor-spawn, actor-send, actor-recv, actor-recv-match)
  |
  +-- compatible with Supervision (spawn-link, spawn-monitor)
  |
  +-- handler procs can use:
        Pattern Matching (let, destructure, match)
        Protocols (type-dispatched handlers)
        Closures (captured configuration)
        Contracts (input validation)
        Transducers (data processing)
```


## 2. Quick Reference

```
gen-server      spec-dict -- coroutine
                % Spawn a GenServer actor.
                % spec-dict must contain /init.
                % Returns coroutine handle.

gen-call        server message -- reply
                % Synchronous call.  Blocks caller until reply.
                % Caller must be an actor (have a mailbox).
                % Raises /type-check if server is not a coroutine.

gen-call-timeout server message ms -- reply
                % Like gen-call, but bounded by ms milliseconds.
                % Raises /limit-check if no reply arrives in time.
                % Caller must be an actor (have a mailbox).

gen-cast        server message --
                % Asynchronous cast.  Fire and forget.
                % Raises /type-check if server is not a coroutine.

gen-reply       from-token reply --
                % Deliver a deferred reply to a caller whose handler
                % returned /noreply.  from-token is the opaque
                % [ref, coroutine] handed to /handle-call as `from`.

gen-stop        server reason --
                % Send graceful shutdown request.
                % Triggers /terminate handler if defined.
                % Raises /type-check if server is not a coroutine.
```

### Spec Dict Keys

| Key | Signature | Required | Description |
| --- | --- | --- | --- |
| `/init` | `-- state` | Yes | Produce initial state |
| `/handle-call` | `msg from state -- state' reply /reply` | No | Handle sync call |
|  |  |  | or: `msg from state -- state' /noreply` |
| `/handle-cast` | `msg state -- state'` | No | Handle async cast |
| `/handle-info` | `msg state -- state'` | No | Handle raw messages |
| `/terminate` | `reason state --` | No | Cleanup before death |


## 3. Usage Patterns

### 3.1 Minimal Server (Init Only)

```
<< /init { 0 } >> gen-server /srv exch def
srv /done gen-stop
```

A server with only `/init` accepts no messages.  Casts and info messages
are silently ignored.  Calls would fail (no `/handle-call`).

### 3.2 Echo Server

```
<<
  /init { null }
  /handle-call {
    [/msg /from /state] let
      msg msg /reply
    end
  }
>> gen-server /echo exch def
```

The `/handle-call` handler receives three arguments: the user's message,
a `from` token (opaque, for deferred replies), and the current state.
It must return: new state, reply value, and the `/reply` sentinel name.

### 3.3 Counter Server (Stateful)

```
<<
  /init { 0 }
  /handle-call {
    [/msg /from /state] let
      state 1 add dup dup /reply
    end
  }
>> gen-server /counter exch def
```

State is threaded automatically.  Each call returns the new count and
updates the internal state.

### 3.4 Accumulator Server (Cast + Call)

```
<<
  /init { 0 }
  /handle-cast { add }                % msg state -- state'
  /handle-call {
    [/msg /from /state] let
      state state /reply              % return state unchanged
    end
  }
>> gen-server /acc exch def

% gen-cast is async and works from anywhere, but gen-call blocks on a
% reply mailbox, so the query side runs inside an actor.
mark {
  % Cast values (async, no reply)
  acc 10 gen-cast
  acc 20 gen-cast
  acc 5 gen-cast

  100 coroutine-sleep     % let casts process

  % Query state (sync)
  acc /query gen-call =   % => 35
} actor-spawn coroutine-join pop
```

### 3.5 Server with Terminate Handler

```
<<
  /init { << /count 0 >> }
  /handle-cast { exch pop /count exch dup /count get 1 add put }
  /terminate { pop pop }    % reason state --
>> gen-server /srv exch def

srv /bye gen-stop           % triggers /terminate
```

`/terminate` runs before the actor dies.  It receives the stop reason and
the final state.  Use it for cleanup (closing resources, flushing buffers).

### 3.6 Server with Info Handler

Raw messages (not wrapped in `/gen-call` or `/gen-cast` protocol) are
dispatched to `/handle-info`:

```
<<
  /init { 0 }
  /handle-info { add }     % raw integer messages add to state
>> gen-server /srv exch def

42 srv actor-send           % raw message via actor-send
```

### 3.7 Complex Reply Values

Replies can be any Trix value -- arrays, tagged values, dicts, records:

```
<<
  /init { 0 }
  /handle-call {
    [/msg /from /state] let
      msg /array eq {
        state [1 2 3] /reply
      } {
        msg /tagged eq {
          state 42 /ok tag /reply
        } {
          state msg /reply
        } if-else
      } if-else
    end
  }
>> gen-server /srv exch def
```

### 3.8 Multiple Independent Servers

```
/echo-spec << /init { null } /handle-call {
    [/msg /from /state] let msg msg /reply end
} >> def

echo-spec gen-server /srv-a exch def
echo-spec gen-server /srv-b exch def

% gen-call blocks on a mailbox, so wrap the calls in an actor.
mark {
  srv-a 111 gen-call =    % => 111
  srv-b 222 gen-call =    % => 222
} actor-spawn coroutine-join pop
```

Each server has independent state.  The spec dict can be reused.

### 3.9 gen-call Must Be Called from an Actor

`gen-call` sends a message and waits for a reply via `actor-recv-match`.
This requires the caller to have a mailbox (be an actor).  The typical
pattern wraps the call in an actor:

```
mark {
  echo-spec gen-server /srv exch def
  srv 42 gen-call         % works: caller is an actor
  srv /done gen-stop
} actor-spawn coroutine-join pop
```

If called from the main coroutine (which is not an actor), the recv will
fail.  Always use `gen-call` from within an actor context.

### 3.10 Timed Synchronous Call

`gen-call-timeout` (`server message ms -- reply`) is `gen-call` with an
upper bound on the wait.  It posts the same internal call message and
waits with `actor-recv-match-timeout` (combines the ref-matching predicate and the timeout):

- If the server replies within `ms` milliseconds, the reply is returned
  exactly as `gen-call` would return it.
- If the deadline passes first, the pending ref is cleaned up and the op
  raises `/limit-check`.  Catch it with `try` to recover.
- `ms` must be a non-negative Integer; a negative value raises
  `/range-check`.

Like `gen-call`, the caller must be an actor.

```
/echo-spec <<
  /init { 0 }
  /handle-call { [/msg /from /state] let state msg /reply end }
>> def

mark {
  echo-spec gen-server /srv exch def

  % Live server replies well within the budget.
  srv 42 5000 gen-call-timeout
  (timed call returned 42) exch 42 eq assert

  % After the server stops, a timed call fails fast instead of blocking.
  srv /done gen-stop
  100 coroutine-sleep
  (dead server times out) { srv 99 100 gen-call-timeout } try /limit-check eq assert
  clear
} actor-spawn coroutine-join pop clear
```

### 3.11 Deferred Reply (`/noreply` + `gen-reply`)

A `/handle-call` handler does not have to answer immediately.  It may
return `state /noreply`, stash the opaque `from` token it was given, and
later deliver the answer with `from-token reply gen-reply`.  Until then
the original `gen-call`er stays blocked -- it is **not** lost, and it is
**not** blocked indefinitely.

The `from` token is the second argument to `/handle-call`: an opaque
`[ref, caller-coroutine]` array.  Pass it unchanged to `gen-reply`.

```
% Server defers the answer on /handle-call, then replies on the next cast.
/deferred-spec <<
  /init { null }
  /handle-call {
    [/msg /from /state] let
      << /pending from /value msg >> /noreply
    end
  }
  /handle-cast {
    [/msg /state] let
      state /pending get state /value get gen-reply
      null
    end
  }
>> def

mark {
  deferred-spec gen-server /srv exch def

  % Caller blocks on the call; it only unblocks after the cast fires.
  mark {
    srv 42 5000 gen-call-timeout
    (deferred reply = 42) exch 42 eq assert
  } actor-spawn /caller exch def

  200 coroutine-sleep
  srv /go gen-cast          % handle-cast runs gen-reply -> caller unblocks
  caller coroutine-join pop

  srv /done gen-stop
  100 coroutine-sleep clear
} actor-spawn coroutine-join pop clear
```

The token's two elements are validated: a non-array, a wrong-length
array, a non-Integer ref, or a non-coroutine target each raise
`/type-check`.


## 4. Real-World Scenarios

### 4.1 Key-Value Store

```
mark {
  <<
    /init { << >> }
    /handle-call {
      [/msg /from /state] let
        msg is-array {
          % Put: msg = [key value].  put consumes the dict and pushes
          % nothing, so re-push state as the new state' (handle-call must
          % return state' reply /reply).
          msg 0 get msg 1 get
          state 3 1 roll put
          state /ok /reply
        } {
          % Get: msg = key
          state msg get
          state exch /reply
        } if-else
      end
    }
  >> gen-server /kv exch def

  kv [/x 42] gen-call % => /ok  (put x=42)
  kv /x gen-call      % => 42   (get x)
} actor-spawn coroutine-join pop
```

`gen-call` must run from inside an actor (it waits on a mailbox), so the
client calls are wrapped in an `actor-spawn` block.

### 4.2 Rate Limiter

```
<<
  /init { << /count 0 /limit 10 >> }
  /handle-call {
    [/msg /from /state] let
      state /count get state /limit get lt {
        state /count state /count get 1 add put
        state true /reply              % allowed
      } {
        state false /reply             % rejected
      } if-else
    end
  }
>> gen-server /limiter exch def
```

### 4.3 Task Queue

```
mark {
  <<
    /init { [] }
    /handle-cast {
      % msg = task, state = queue.  append returns the new array.
      exch append             % enqueue task
    }
    /handle-call {
      [/msg /from /state] let
        msg /dequeue eq {
          state length 0 gt {
            state 1 drop                 % rest of queue (new state')
            state 0 get                  % first task (reply)
            /reply
          } {
            state null /reply            % empty
          } if-else
        } {
          state state /reply             % default: return queue
        } if-else
      end
    }
  >> gen-server /q exch def

  q /a gen-cast
  q /b gen-cast
  q /c gen-cast
  100 coroutine-sleep                    % let casts process

  q /dequeue gen-call    % => /a
  q /dequeue gen-call    % => /b
  q /done gen-stop
} actor-spawn coroutine-join pop
```

`append` (`arr any -- arr`) is the array-append op; `concat` is
string-only.  `state 1 drop` slices off the head to leave the remaining
queue as the new state.

### 4.4 Event Aggregator

```
mark {
  <<
    /init { [] }
    /handle-cast {
      % Cast events into the server (append returns the new array)
      exch append             % accumulate
    }
    /handle-call {
      % Call to retrieve and reset
      [/msg /from /state] let
        msg /flush eq {
          []                  % new state: empty
          state /reply        % reply: all accumulated events
        } {
          state state /reply
        } if-else
      end
    }
  >> gen-server /agg exch def

  agg 1 gen-cast
  agg 2 gen-cast
  agg 3 gen-cast
  100 coroutine-sleep

  agg /flush gen-call    % => [1 2 3]   (then state resets to [])
  agg /done gen-stop
} actor-spawn coroutine-join pop
```


## 5. Design Choices

### 5.1 Internal Message Protocol (Not User-Visible)

GenServer wraps user messages in internal arrays:

| Type  | Format                                           |
| ----- | ------------------------------------------------ |
| Call  | `[/gen-call, ref, from-coroutine, user-message]` |
| Cast  | `[/gen-cast, user-message]`                      |
| Stop  | `[/gen-stop, reason]`                            |
| Reply | `[ref, reply-value]`                             |

**Why**: Users never see these wrappers.  `gen-call` builds the call
message; `gen-cast` builds the cast message; the GenServer recv loop
unwraps them.  Because names are globally interned, a user-sent raw
`[/gen-call ...]` (or `[/gen-cast ...]`) array via `actor-send` WILL be
dispatched as a call (resp. cast) -- the protocol is **not** isolated or
unforgeable; pre-interned exact matching is precisely why a forged tag
matches.  Only arrays whose element 0 is some other name (or a non-name)
fall through to `/handle-info`.

### 5.2 /reply and /noreply Sentinels

`handle-call` must end with either:
- `state' reply /reply` -- send reply immediately
- `state' /noreply` -- defer reply (caller stays blocked)

**Why**: Explicit sentinels make the control flow visible.  The GenServer
loop checks the sentinel after the handler returns to know whether to
send a reply.  This is the Erlang `{reply, Reply, State}` /
`{noreply, State}` pattern, adapted to stack semantics.

### 5.3 Trampoline-Safe actor-send

All `actor-send` calls in GenServer code use the trampoline pattern:
push the `ActorSend` operator onto the exec stack, set up the operand
stack, and return.  The interpreter then executes `ActorSend` in its
normal loop.

**Why**: `actor-send` can block if the target's mailbox is full.  Blocking
triggers a coroutine context switch.  If `actor-send` were called as a
direct C++ function from within a GenServer handler, the context switch
would invalidate all local variables (stack pointers, frame pointers).
The trampoline avoids this by ensuring all state is on the VM stacks
(not C++ local variables) before the potentially-blocking operation.

### 5.4 4-Slot Exec Stack Frame

The GenServer's persistent state lives in 4 exec stack slots:

```
frame[-3] = spec-dict
frame[-2] = state (updated each iteration)
frame[-1] = call-ref (pending call ref, null when idle)
frame[0]  = call-from (pending caller handle, null when idle)
```

**Why**: The exec stack is the natural place for persistent loop state in
Trix.  Coroutine context switches save/restore the exec stack, so the
frame automatically persists across yields.  No heap allocation needed
for the loop state beyond the initial setup.

### 5.5 Pre-Interned Handler Names

Handler keys (`/init`, `/handle-call`, `/handle-cast`, `/handle-info`,
`/terminate`) are pre-interned well-known names (`WellKnownName::Init`,
`HandleCall`, `HandleCast`, `HandleInfo`, `Terminate`), reached via
`wellknown_name(...)`.

**Why**: Avoids Name::make hash computation on every message receive.
The GenServer recv loop runs for every message; pre-interned names make
the spec dict lookup a fast hash comparison instead of a string hash +
lookup.

### 5.6 gen-call-timeout

`gen-call-timeout` (`server message ms -- reply`) is implemented.  It is
like `gen-call` but bounds the wait: if the server does not reply within
`ms` milliseconds it raises `/limit-check`.  It builds on
`actor-recv-match-timeout`, cleaning up the pending ref when
the timeout fires.  See §3.10 for a worked example.

### 5.7 gen-reply

`gen-reply` (`from-token reply --`) is implemented.  It powers the
deferred-reply (`/noreply`) pattern: a `handle-call` handler receives an
opaque `from` token (ref + caller coroutine), may return `state /noreply`
to defer, and later delivers the answer with `from reply gen-reply`.
See §3.11 for a worked example.


## 6. Implementation Internals

### 6.1 Startup Sequence

```
gen-server: spec-dict -- coroutine

1. Verify spec-dict has /init.
2. Build startup proc: [spec-literal, mark, init-proc, exec, @gen-server-init] (5 elements)
3. Push mark + startup proc on op stack.
4. Delegate to actor-spawn (creates actor with mailbox).

In the new coroutine:
5. spec-literal pushes spec-dict onto op stack.
6. init-proc executes, leaving state on op stack.
7. @gen-server-init: pop spec + state, build 4-slot exec frame,
   start recv loop.
```

### 6.2 Receive Loop

```
Each iteration:
1. actor-recv blocks until message arrives.
2. @gen-server-recv fires:
   a. Categorize message (call, cast, stop, or info).
   b. Look up handler in spec-dict.
   c. Push handler args on op stack.
   d. Push handler + done-op on exec stack.
   e. Handler executes.
3. Done-op fires (@gen-server-call-done or @gen-server-cast-done):
   a. Pop new state from op stack.
   b. Update state in exec frame.
   c. For calls: build reply message, send to caller.
   d. Restart recv loop.
```

### 6.3 gen-call Mechanics (Caller Side)

```
gen-call: server message -- reply

1. Generate unique ref (m_gen_ref_counter++).
2. Get self (caller's coroutine handle).
3. Build call message: [/gen-call, ref, self, message].
4. Build ref-matching predicate:
   curry [ref, { exch dup is-array { 0 get eq } { pop pop false } if-else }]
5. Build reply extractor: { 1 get }
6. Set up exec stack (LIFO):
   actor-send -> pred(literal) -> actor-recv-match -> { 1 get }
7. Set up op stack: call_msg, server (for actor-send).

Execution:
8. actor-send: send call message to server.
9. pred pushed to op stack (literal).
10. actor-recv-match: scan mailbox for [ref, *] message.
11. { 1 get }: extract reply value from [ref, reply].
```

The ref-matching predicate is a Curry object.  When `actor-recv-match`
tests a message:
1. Curry pushes ref onto op stack, then executes the body.
2. Body: `exch dup is-array { 0 get eq } { pop pop false } if-else`
   - If message is an array: compare msg[0] with ref.
   - Otherwise: return false.
3. Only the reply message (which has the matching ref) passes.

### 6.4 gen-stop Mechanics

```
gen-stop: server reason --

1. Build stop message: [/gen-stop, reason].
2. Push actor-send on exec stack (trampoline-safe).
3. Set up op stack: stop_msg, server.

Server side (@gen-server-recv):
4. Recognize /gen-stop tag.
5. If /terminate handler exists:
   Push reason + state on op stack.
   Push die + terminate on exec stack.
   Free exec frame ExtValues.
6. If no /terminate:
   Free exec frame ExtValues.
   Call die_op directly.
```

### 6.5 Memory Cost

| Component                                   | Size                                    |
| ------------------------------------------- | --------------------------------------- |
| Startup proc (5 elements)                   | 40 bytes                                |
| Exec frame (4 slots)                        | 32 bytes (on exec stack, no heap alloc) |
| Call message (4 elements)                   | 32 bytes                                |
| Cast message (2 elements)                   | 16 bytes                                |
| Reply message (2 elements)                  | 16 bytes                                |
| Ref-matching predicate (Curry + body procs) | ~160 bytes (one-time, per gen-call)     |
| Per-server overhead                         | ~72 bytes (startup + frame)             |

The dominant cost is the actor's mailbox (allocated by `actor-spawn`),
not the GenServer machinery.

### 6.6 Ref Counter

`m_gen_ref_counter` is a `uint64_t` member variable.  Each `gen-call`
increments it.  At 2^64 calls, it wraps -- effectively inexhaustible.

### 6.7 Snap-Shot/Thaw

The `m_gen_ref_counter` member is saved in `SnapShotHeader`, and the
pre-interned handler names round-trip through the well-known offset table.
The exec frame lives on the coroutine's exec stack, which is captured by
the coroutine's snap-shot.  GenServer state survives snap-shot/thaw.

### 6.8 Source File

All GenServer operators are in `src/ops_genserver.inl` (~734 lines).
The pre-interned handler names are `WellKnownName` entries in
`src/enums.inl`; the ref counter (`m_gen_ref_counter`) is in
`src/member_vars.inl`.


## 7. Composability

### 7.1 GenServer + Supervision

GenServers are actors.  Actors can be supervised:

```
% Spawn GenServer under supervisor (conceptual)
/child-spec <<
  /init { 0 }
  /handle-call { ... }
>> def

child-spec gen-server    % returns coroutine
% ... link/monitor with supervisor ...
```

The supervision framework's `spawn-link` and `spawn-monitor` work with
any coroutine, including GenServers.

### 7.2 GenServer + Pattern Matching

Use `let` and `destructure` in handlers for readable argument binding:

```
/handle-call {
  [/msg /from /state] let
    msg [
      { tag-name /get eq }  { tag-value state exch get state exch /reply }
      { tag-name /put eq }  { tag-value [/k /v] destructure
                                    state k v put /ok /reply end }
      { pop true }              { pop state /unknown /reply }
    ] match
  end
}
```

### 7.3 GenServer + Protocols

Dispatch on message type via protocols:

```
[/process] /Processable def-protocol

{ tag-value 2 mul } /process /tagged-type def-method
{ 1 add }           /process /integer-type def-method

/handle-cast {
  exch process    % dispatch via protocol
  add             % add result to state
}
```

### 7.4 GenServer + Closures

Capture configuration for handlers:

```
/max-items 1000 def

/handle-cast {
  exch append
  dup length max-items gt { max-items take } if
} [/max-items] closure-capture
```

### 7.5 GenServer + Contracts

Guard handler inputs:

```
/handle-call {
  [/msg /from /state] let
    msg dup is-integer precondition
    state add dup dup /reply
  end
}
```

### 7.6 GenServer + Transducers

Process batch messages with transducers:

```
/handle-call {
  [/msg /from /state] let
    msg /batch eq {
      state { 2 mod 0 eq } xf-filter into
      state exch /reply
    } { state msg /reply } if-else
  end
}
```


## 8. Error Handling

| Error          | Condition                                                               |
| -------------- | ----------------------------------------------------------------------- |
| `/undefined`   | spec-dict missing `/init` (`gen-server`)                                |
| `/undefined`   | Call received but no `/handle-call` in spec                             |
| `/type-check`  | Server argument is not a coroutine (`gen-call`, `gen-cast`, `gen-stop`) |
| `/type-check`  | `handle-call` returns neither `/reply` nor `/noreply`                   |
| `/range-check` | Malformed call message (fewer than 4 elements)                          |
| `/limit-check` | `gen-call-timeout`: server did not reply within the timeout             |
| `/range-check` | `gen-call-timeout`: negative timeout `ms`                               |
| `/type-check`  | `gen-reply`: `from-token` is not a `[ref, coroutine]` token             |

### Error in Handler

If a handler proc raises an error, the actor crashes.  If the actor is
supervised (linked or monitored), the supervisor receives a death
notification and can restart it per the supervision strategy.  This is
the Erlang "let it crash" philosophy -- handlers should not catch errors
that they cannot meaningfully handle.


## 9. Limitations

- **gen-call requires actor context.** The caller must be an actor (have
  a mailbox) to receive the reply.  Calling `gen-call` from the main
  coroutine will fail.  Wrap calls in `actor-spawn`.

- **gen-call-timeout** (`server message ms -- reply`) bounds the sync-call
  wait, raising `/limit-check` if the server does not reply within `ms`
  milliseconds.

- **gen-reply** (`from-token reply --`) delivers deferred replies: a
  handler may return `state /noreply` and later answer the saved `from`
  token with `gen-reply`, so a caller using `/noreply` is not blocked
  indefinitely.

- **There is no gen-state operator (by design).** Add a `/query` arm
  to `/handle-call` that returns the state.  This is the idiomatic
  pattern -- state introspection belongs in the handler, not in
  cross-coroutine exec-stack inspection.

- **No automatic supervision.** `gen-server` does not auto-link to the
  caller.  Use the supervision framework explicitly.

- **Single handler per message type.** There is no middleware or
  interceptor chain.  All dispatch logic goes in the handler proc.

- **No hot code swap.** The spec dict is captured at spawn time.  Changing
  handlers requires stopping and restarting the server.
