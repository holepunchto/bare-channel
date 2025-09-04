const EventEmitter = require('bare-events')
const { Readable, Writable } = require('bare-stream')
const structuredClone = require('bare-structured-clone')
const FIFO = require('fast-fifo')
const binding = require('./binding')

const MAX_BUFFER = 128

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

    this._closing = null
    this._buffer = new FIFO()
    this._backpressured = false

    this._drainedPromise = null
    this._drainedQueue = (resolve) => {
      this._ondrained = resolve
    }

    this._waitPromise = null
    this._waitQueue = (resolve) => {
      this._onwait = resolve
    }

    this._onwait = null
    this._ondrained = null
    this._onclosed = null
    this._onremoteclose = null

    this._id = binding.portInit(
      channel.handle,
      this,
      this._ondrain,
      this._onflush,
      this._onend,
      this._onclose
    )

    this.closed = false
    this.remoteClosed = false
  }

  get buffered() {
    return this._buffer.length
  }

  get drained() {
    return this._drainedPromise !== null
  }

  get closing() {
    return this._closing !== null
  }

  async read() {
    do {
      if (this._buffer.length > 0) return this._buffer.shift()
    } while (await this._wait())

    return null
  }

  readSync() {
    while (true) {
      if (this._closing !== null) return null

      if (this._buffer.length > 0) return this._buffer.shift()

      binding.portWait(this._channel.handle, this._id)

      this._onflush()
    }
  }

  async write(value, opts = {}) {
    if (value === null) return

    const serialized = structuredClone.serializeWithTransfer(
      value,
      opts.transfer,
      this._channel.interfaces
    )

    const state = { start: 0, end: 0, buffer: null }

    structuredClone.preencode(state, serialized)

    const data = (state.buffer = Buffer.allocUnsafe(state.end))

    structuredClone.encode(state, serialized)

    while (true) {
      while (this._drainedPromise !== null) await this._drainedPromise

      if (this._closing !== null) return false

      if (binding.portWrite(this._channel.handle, this._id, data.buffer)) break

      if (this._drainedPromise === null) {
        this._drainedPromise = new Promise(this._drainedQueue)
      }
    }
  }

  async *[Symbol.asyncIterator]() {
    do {
      while (this._buffer.length > 0) yield this._buffer.shift()
    } while (await this._wait())
  }

  createReadStream(opts) {
    return new PortReadStream(this, opts)
  }

  createWriteStream(opts) {
    return new PortWriteStream(this, opts)
  }

  close() {
    if (this._closing === null) this._closing = this._close()
    return this._closing
  }

  async _close() {
    await Promise.resolve() // Avoid re-entry

    this.emit('closing')

    while (this._drainedPromise !== null) await this._drainedPromise

    binding.portEnd(this._channel.handle, this._id)

    if (!this.remoteClosed) {
      await new Promise((resolve) => {
        this._onremoteclose = resolve
      })
    }

    this._onremoteclose = null

    const destroyed = new Promise((resolve) => {
      this._onclosed = resolve
    })

    binding.portDestroy(this._channel.handle, this._id)

    await destroyed

    this._onclosed = null

    this.closed = true
    this.emit('close')
  }

  ref() {
    binding.portRef(this._channel.handle, this._id)
  }

  unref() {
    binding.portUnref(this._channel.handle, this._id)
  }

  _wait() {
    if (this._backpressured) this._onflush()

    if (this._closing !== null || this._buffer.length > 0) {
      return Promise.resolve(this._closing === null)
    }

    if (this._waitPromise === null) {
      this._waitPromise = new Promise(this._waitQueue)
    }

    return this._waitPromise
  }

  _ondrain() {
    if (this._ondrained === null) return

    const ondrained = this._ondrained
    this._ondrained = null
    this._drainedPromise = null

    ondrained(this._closing === null)
  }

  _onflush() {
    this._backpressured = false

    while (this._buffer.length < MAX_BUFFER) {
      const data = binding.portRead(this._channel.handle, this._id)

      if (data === null) return

      const state = {
        start: 0,
        end: data.byteLength,
        buffer: Buffer.from(data)
      }

      const value = structuredClone.deserializeWithTransfer(
        structuredClone.decode(state),
        this._channel.interfaces
      )

      this._buffer.push(value)
      this._onactive()
    }

    this._backpressured = true
  }

  _onactive() {
    if (this._onwait === null) return

    const onwait = this._onwait
    this._onwait = null
    this._waitPromise = null

    onwait(this._closing === null)
  }

  _onend() {
    this.remoteClosed = true

    if (this._onremoteclose !== null) this._onremoteclose()
    else this.close()
  }

  _onclose() {
    this._onclosed()
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
