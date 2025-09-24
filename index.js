const EventEmitter = require('bare-events')
const { Readable, Writable } = require('bare-stream')
const structuredClone = require('bare-structured-clone')
const binding = require('./binding')
const Queue = require('./lib/queue')

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

    this._queue = new Queue()
    this._backpressured = false

    this._draining = null
    this._flushing = null
    this._closing = null

    this._id = binding.portInit(
      channel.handle,
      this,
      this._ondrain,
      this._onflush,
      this._onend,
      this._onclose
    )
  }

  async read() {
    while (this._flushing !== null) await this._flushing.promise

    while (true) {
      if (this._backpressured) this._onflush()

      if (this._queue.length > 0) return this._queue.shift()

      this._flushing = Promise.withResolvers()

      await this._flushing.promise
    }
  }

  readSync() {
    while (true) {
      if (this._queue.length > 0) return this._queue.shift()

      binding.portWaitFlush(this._channel.handle, this._id)

      this._onflush()
    }
  }

  async write(value, opts = {}) {
    if (value === null) return false

    while (this._draining !== null) await this._draining.promise

    if (this._closing !== null) return false

    const data = encode(this._channel, value, opts)

    while (true) {
      const flushed = binding.portWrite(this._channel.handle, this._id, data)

      if (flushed) return true

      this._draining = Promise.withResolvers()

      await this._draining.promise
    }
  }

  writeSync(value, opts = {}) {
    if (value === null) return false

    const data = encode(this._channel, value, opts)

    while (true) {
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

  async close() {
    while (this._draining !== null) await this._draining.promise

    if (this._closing !== null) return this._closing.promise

    binding.portEnd(this._channel.handle, this._id)

    this._closing = Promise.withResolvers()

    await this._closing.promise
  }

  ref() {
    if (this._closing !== null) return

    binding.portRef(this._channel.handle, this._id)
  }

  unref() {
    if (this._closing !== null) return

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
    if (this._draining === null) return

    const draining = this._draining
    this._draining = null
    draining.resolve()
  }

  _onflush() {
    while (this._queue.length < this._queue.capacity) {
      const data = binding.portRead(this._channel.handle, this._id)

      if (data === null) break

      this._queue.push(decode(this._channel, data))
    }

    this._backpressured = this._queue.length === this._queue.capacity

    if (this._flushing === null) return

    const flushing = this._flushing
    this._flushing = null
    flushing.resolve()
  }

  _onend() {
    this.emit('end')
  }

  _onclose() {
    if (this._closing === null) this._closing = Promise.withResolvers()

    const closing = this._closing
    this._closing = null
    closing.resolve()

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
