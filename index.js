const events = require('bare-events')
const FIFO = require('fast-fifo')
const binding = require('./binding')

const MAX_BUFFER = 128

module.exports = exports = class Channel {
  constructor (opts = {}) {
    const {
      handle = binding.channelInit()
    } = opts

    this.handle = handle
  }

  connect () {
    return new Port(this)
  }

  destroy () {
    binding.channelDestroy(this.handle)
  }

  static from (handle, opts = {}) {
    return new Channel({ ...opts, handle })
  }
}

class Port extends events.EventEmitter {
  constructor (channel) {
    super()

    this.closed = false
    this.remoteClosed = false

    this._closing = null
    this._buffer = new FIFO()
    this._backpressured = false

    this._drainedPromise = null
    this._drainedQueue = (resolve) => { this._ondrained = resolve }

    this._waitPromise = null
    this._waitQueue = (resolve) => { this._onwait = resolve }

    this._onwait = null
    this._ondrained = null
    this._ondestroyed = null
    this._onremoteclose = null

    this.handle = binding.portInit(channel.handle, this,
      this._ondrain,
      this._onflush,
      this._onend,
      this._ondestroy
    )
  }

  get buffered () {
    return this._buffer.length
  }

  get drained () {
    return this._drainedPromise !== null
  }

  get closing () {
    return this._closing !== null
  }

  async send (value) {
    if (typeof value === 'string') value = Buffer.from(value)

    while (true) {
      while (this._drainedPromise !== null) await this._drainedPromise

      if (this._closing !== null) return false
      if (binding.portWrite(this.handle, value)) break

      if (this._drainedPromise === null) this._drainedPromise = new Promise(this._drainedQueue)
    }
  }

  _recvSync () {
    while (true) {
      if (this._closing !== null) return null

      if (this._buffer.length === 0) {
        binding.portWait(this.handle)
        this._onflush()
        continue
      }

      return this._buffer.shift()
    }
  }

  async _recvAsync () {
    do {
      if (this._buffer.length) return this._buffer.shift()
    } while (await this._wait())

    return null
  }

  recv (sync = false) {
    return sync ? this._recvSync() : this._recvAsync()
  }

  async * [Symbol.asyncIterator] () {
    do {
      while (this._closing === null && this._buffer.length > 0) {
        yield this._buffer.shift()
      }
    } while (await this._wait())
  }

  close () {
    if (this._closing === null) this._closing = this._close()
    return this._closing
  }

  async _close () {
    await Promise.resolve() // force one tick to avoid re-entry
    this.emit('closing')

    // drain any pending writes
    while (this._drainedPromise !== null) await this._drainedPromise

    binding.portEnd(this.handle)

    // wait for the remote to signal end also
    if (!this.remoteClosed) await new Promise((resolve) => { this._onremoteclose = resolve })
    this._onremoteclose = null

    // now destroy
    const destroyed = new Promise((resolve) => { this._ondestroyed = resolve })
    binding.portDestroy(this.handle)
    await destroyed
    this._ondestroyed = null

    this.closed = true
    this.emit('close')
  }

  _wait () {
    if (this._backpressured) this._onflush()
    if (this._buffer.length > 0 || this._closing !== null) return Promise.resolve(this._closing === null)
    if (!this._waitPromise) this._waitPromise = new Promise(this._waitQueue)
    return this._waitPromise
  }

  _ondrain () {
    if (this._ondrained === null) return

    const ondrained = this._ondrained
    this._ondrained = null
    this._drainedPromise = null

    ondrained(this._closing === null)
  }

  _onflush () {
    this._backpressured = false

    while (this._buffer.length < MAX_BUFFER) {
      const value = binding.portRead(this.handle)
      if (value === null) return
      this._buffer.push(ArrayBuffer.isView(value) ? Buffer.coerce(value) : value)
      this._onactive()
    }

    this._backpressured = true
  }

  _onactive () {
    if (this._onwait === null) return

    const onwait = this._onwait
    this._onwait = null
    this._waitPromise = null

    onwait(this._closing === null)
  }

  _onend () {
    this.remoteClosed = true

    if (this._onremoteclose !== null) this._onremoteclose()
    else this.close() // run in bg
  }

  _ondestroy () {
    this._ondestroyed()
  }
}
