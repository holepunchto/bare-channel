const events = require('bare-events')
const FIFO = require('fast-fifo')
const binding = require('./binding')

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

    this.drained = true
    this.closed = false
    this.remoteClosed = false

    this._closing = null
    this._send = new FIFO()
    this._recv = new FIFO()

    this._readablePromise = null
    this._readableQueue = (resolve) => { this._onreadable = resolve }

    this._onreadable = null
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

  get closing () {
    return this._closing !== null
  }

  send (value) {
    if (this._closing !== null) return
    if (typeof value === 'string') value = Buffer.from(value)

    if (!this.drained) {
      this._send.push(value)
      return
    }

    this.drained = binding.portWrite(this.handle, value)
  }

  recv (wait = false) {
    if (this._closing !== null) return null

    if (this._recv.length === 0) return null

    return this._recv.shift()
  }

  close () {
    if (this._closing === null) this._closing = this._close()
    return this._closing
  }

  async _close () {
    await Promise.resolve() // force one tick to avoid re-entry
    this.emit('closing')

    // drain any pending writes
    if (!this.drained) await new Promise((resolve) => { this._ondrained = resolve })
    this._ondrained = null

    binding.portEnd(this.handle)

    // wait for the remote to signal end also
    if (!this.remoteClosed) await new Promise((resolve) => { this._onremoteclose = resolve })
    this._onremoteclose = null

    // now destroy
    const destroyed = new Promise((resolve) => { this._ondestroyed = resolve })
    binding.portDestroy(this.handle)
    await destroyed

    this.closed = true
    this.emit('close')
  }

  readable () {
    if (this._recv.length > 0) return Promise.resolve(this._closing === null)
    if (!this._readablePromise) this._readablePromise = new Promise(this._readableQueue)
    return this._readablePromise
  }

  async * [Symbol.asyncIterator] () {
    do {
      while (true) {
        const data = this.recv(false)
        if (data === null) break
        yield data
      }
    } while (await this.readable())
  }

  _ondrain () {
    this.drained = true
    while (this.drained && this._send.length > 0) this.send(this._send.shift())
    if (this.drained && this._ondrained) this._ondrained()
  }

  _onflush () {
    while (true) {
      const value = binding.portRead(this.handle)
      if (value === null) break
      this._recv.push(ArrayBuffer.isView(value) ? Buffer.coerce(value) : value)
      this._readable()
    }

    if (this._recv.length > 0) this.emit('recv')
  }

  _readable () {
    if (this._onreadable === null) return

    const onnotify = this._onreadable
    this._onreadable = null
    this._readablePromise = null

    onnotify(this._recv.length > 0 && this._closing === null)
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
