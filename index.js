const EventEmitter = require('bare-events')
const { Readable, Writable, Duplex } = require('bare-stream')
const structuredClone = require('bare-structured-clone')
const binding = require('./binding')
const Queue = require('./lib/queue')

const ENDED = 0x1
const REMOTE_ENDED = 0x2

module.exports = exports = class Channel {
  constructor(opts = {}) {
    const { handle = binding.channelInit(), interfaces = [] } = opts

    this.handle = handle
    this.interfaces = interfaces
  }

  connect() {
    return new Port(this)
  }

  static from(handle, opts = {}) {
    return new Channel({ ...opts, handle })
  }
}

class Port extends EventEmitter {
  constructor(channel) {
    super()

    this._channel = channel
    this._state = 0
    this._queue = new Queue()
    this._backpressured = false

    this._drain = null
    this._flush = null
    this._end = null
    this._remoteend = null
    this._close = null

    this._id = binding.portInit(
      channel.handle,
      this,
      this._ondrain,
      this._onflush,
      this._onend,
      this._onremoteend,
      this._onclose
    )
  }

  async read() {
    while (this._flush !== null) await this._flush.promise

    while (true) {
      if (this._backpressured) this._onflush()

      if (this._queue.length > 0) return this._queue.shift()

      if (this._state & REMOTE_ENDED) return null

      this._flush = Promise.withResolvers()

      await this._flush.promise
    }
  }

  readSync() {
    while (true) {
      if (this._queue.length > 0) return this._queue.shift()

      if (this._state & REMOTE_ENDED) return null

      binding.portWaitFlush(this._channel.handle, this._id)

      this._onflush()
    }
  }

  async write(value, opts = {}) {
    if (value === null) return false

    while (this._drain !== null) await this._drain.promise

    if (this._close !== null) return false

    const data = encode(this._channel, value, opts)

    while (true) {
      if (this._state & REMOTE_ENDED) return false

      const flushed = binding.portWrite(this._channel.handle, this._id, data)

      if (flushed) return true

      this._drain = Promise.withResolvers()

      await this._drain.promise
    }
  }

  writeSync(value, opts = {}) {
    if (value === null) return false

    const data = encode(this._channel, value, opts)

    while (true) {
      if (this._state & REMOTE_ENDED) return false

      const flushed = binding.portWrite(this._channel.handle, this._id, data)

      if (flushed) return true

      binding.portWaitDrain(this._channel.handle, this._id)
    }
  }

  createReadStream(opts) {
    return new PortReadStream(this, opts)
  }

  createWriteStream(opts) {
    return new PortWriteStream(this, opts)
  }

  createStream(opts) {
    return new PortDuplexStream(this, opts)
  }

  async close() {
    while (this._drain !== null) await this._drain.promise

    if (this._close !== null) return this._close.promise

    this._state |= ENDED
    this._close = Promise.withResolvers()

    while (true) {
      const flushed = binding.portEnd(this._channel.handle, this._id)

      if (flushed) break

      this._drain = Promise.withResolvers()

      await this._drain.promise
    }

    if (this._end === null) this._end = Promise.withResolvers()
    if (this._remoteend === null) this._remoteend = Promise.withResolvers()

    await this._end.promise
    await this._remoteend.promise

    binding.portClose(this._channel.handle, this._id)

    await this._close.promise
  }

  ref() {
    if (this._close !== null) return

    binding.portRef(this._channel.handle, this._id)
  }

  unref() {
    if (this._close !== null) return

    binding.portUnref(this._channel.handle, this._id)
  }

  *[Symbol.iterator]() {
    while (true) {
      const data = this.readSync()
      if (data === null) break
      yield data
    }
  }

  async *[Symbol.asyncIterator]() {
    while (true) {
      const data = await this.read()
      if (data === null) break
      yield data
    }
  }

  _ondrain() {
    if (this._drain === null) return

    const draining = this._drain
    this._drain = null
    draining.resolve()
  }

  _onflush() {
    while (this._queue.length < this._queue.capacity) {
      const data = binding.portRead(this._channel.handle, this._id)

      if (data === null) break

      this._queue.push(decode(this._channel, data))
    }

    this._backpressured = this._queue.length === this._queue.capacity

    if (this._flush === null) return

    const flushing = this._flush
    this._flush = null
    flushing.resolve()
  }

  _onend() {
    if (this._end === null) this._end = Promise.withResolvers()

    this._state |= ENDED
    this._end.resolve()
  }

  _onremoteend() {
    if (this._remoteend === null) this._remoteend = Promise.withResolvers()

    this._state |= REMOTE_ENDED
    this._remoteend.resolve()

    this.close()
    this.emit('end')
  }

  _onclose() {
    if (this._close === null) this._close = Promise.withResolvers()

    this._close.resolve()

    this.emit('close')
  }
}

class PortReadStream extends Readable {
  constructor(port, opts) {
    super(opts)

    this._port = port
  }

  async _read() {
    try {
      this.push(await this._port.read())
    } catch (err) {
      this.destroy(err)
    }
  }
}

class PortWriteStream extends Writable {
  constructor(port, opts) {
    super(opts)

    this._port = port
  }

  async _write(chunk, encoding, cb) {
    let err = null
    try {
      await this._port.write(chunk)
    } catch (e) {
      err = e
    }

    cb(err)
  }

  async _final(cb) {
    let err = null
    try {
      await this._port.close()
    } catch (e) {
      err = e
    }

    cb(err)
  }
}

class PortDuplexStream extends Duplex {
  constructor(port, opts) {
    super(opts)

    this._port = port
  }

  async _read() {
    try {
      this.push(await this._port.read())
    } catch (err) {
      this.destroy(err)
    }
  }

  async _write(chunk, encoding, cb) {
    let err = null
    try {
      await this._port.write(chunk)
    } catch (e) {
      err = e
    }

    cb(err)
  }

  async _final(cb) {
    let err = null
    try {
      await this._port.close()
    } catch (e) {
      err = e
    }

    cb(err)
  }
}

function encode(channel, value, opts) {
  const serialized = structuredClone.serializeWithTransfer(
    value,
    opts.transfer,
    channel.interfaces
  )

  const state = { start: 0, end: 0, buffer: null }

  structuredClone.preencode(state, serialized)

  const data = new ArrayBuffer(state.end)

  state.buffer = Buffer.from(data)

  structuredClone.encode(state, serialized)

  return data
}

function decode(channel, data) {
  const state = {
    start: 0,
    end: data.byteLength,
    buffer: Buffer.from(data)
  }

  return structuredClone.deserializeWithTransfer(
    structuredClone.decode(state),
    channel.interfaces
  )
}
