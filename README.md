# bare-channel

Inter-thread messaging for JavaScript. Messages are serialized using the structured clone algorithm provided by <https://github.com/holepunchto/bare-structured-clone>, allowing arbitrary values and transferable objects to be exchanged between threads.

```
npm i bare-channel
```

## Usage

```js
const Channel = require('bare-channel')
const { Thread } = Bare

const channel = new Channel()

const thread = new Thread(__filename, { data: channel.handle }, async (handle) => {
  const Channel = require('bare-channel')

  const channel = Channel.from(handle)
  const port = channel.connect()

  for await (const data of port) {
    await port.write(data)
  }
})

const port = channel.connect()

await port.write('ping')

console.log(await port.read())

await port.close()

thread.join()
```

## API

#### `const channel = new Channel([options])`

Create a new channel. A channel is a shared rendezvous point between threads; both ends acquire a `Port` by calling `channel.connect()` to send and receive messages.

`options` include:

```js
options = {
  handle,
  interfaces: []
}
```

`handle` is an existing `SharedArrayBuffer` returned by `channel.handle` on another thread. When provided, the new channel is wired up to the same underlying channel. If omitted, a fresh channel is created.

`interfaces` is an array of constructors with `bare-structured-clone` serialize and/or transfer symbols. Instances of these types may be passed to `port.write()` and will be reconstructed on the receiving side. See <https://github.com/holepunchto/bare-structured-clone> for the symbols and conventions.

#### `channel.handle`

The underlying `SharedArrayBuffer` for the channel. Pass this across thread boundaries (e.g. via `Bare.Thread`'s `data` option) and reconstruct the channel on the other side with `Channel.from(handle)`.

#### `channel.interfaces`

The array of constructors passed to the constructor.

#### `const port = channel.connect()`

Connect a new `Port` to the channel. A channel supports two connected ports; messages written on one port appear in the read queue of the other.

#### `const channel = Channel.from(handle[, options])`

Static helper for constructing a channel from an existing `handle`. Equivalent to `new Channel({ ...options, handle })`.

#### `const data = await port.read()`

Read the next value from the port. Resolves with `null` once the remote side has ended and the queue is drained.

#### `const data = port.readSync()`

Synchronous variant of `port.read()` that blocks the current thread until a value is available or the remote side has ended. Returns `null` when the remote side has ended and the queue is drained.

#### `const flushed = await port.write(value[, options])`

Write `value` to the port. Resolves to `true` once the value has been accepted into the channel, or `false` if the remote side has ended.

`options` include:

```js
options = {
  transfer: []
}
```

`transfer` is an array of transferable values that should be transferred to the receiving side rather than cloned, following the structured clone algorithm.

#### `const flushed = port.writeSync(value[, options])`

Synchronous variant of `port.write()` that blocks the current thread when the channel is full until the value can be enqueued. Returns `false` if the remote side has ended.

#### `const stream = port.createReadStream([options])`

Create a `Readable` stream that consumes values from the port. `options` are forwarded to <https://github.com/holepunchto/bare-stream>.

#### `const stream = port.createWriteStream([options])`

Create a `Writable` stream that writes values to the port. The port is closed when the stream is finalized. `options` are forwarded to <https://github.com/holepunchto/bare-stream>.

#### `const stream = port.createStream([options])`

Create a `Duplex` stream that both reads from and writes to the port. The port is closed when the stream is finalized. `options` are forwarded to <https://github.com/holepunchto/bare-stream>.

#### `await port.close()`

Close the port. Pending writes are flushed before the close completes. After the port is closed, further writes resolve to `false` and reads return `null` once the queue is drained.

#### `port.ref()`

Reference the port, keeping the event loop alive while it is open.

#### `port.unref()`

Unreference the port, allowing the event loop to exit even if the port is still open. The port will be closed automatically when no other handles are keeping the event loop alive.

#### `for (const data of port)`

Synchronously iterate over values read from the port. Equivalent to repeatedly calling `port.readSync()` until it returns `null`.

#### `for await (const data of port)`

Asynchronously iterate over values read from the port. Equivalent to repeatedly calling `port.read()` until it returns `null`.

#### `event: 'end'`

Emitted when the remote side has ended.

#### `event: 'close'`

Emitted when the port has been closed.

## License

Apache-2.0
