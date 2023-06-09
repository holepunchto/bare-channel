const { Duplex } = require('streamx')
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

class Port extends Duplex {
  constructor (channel) {
    super({ mapWritable })

    this._pendingWrite = null
    this._pendingEnd = null
    this._pendingDestroy = null

    this.handle = binding.portInit(channel.handle, this,
      this._ondrain,
      this._onflush,
      this._onend,
      this._ondestroy
    )
  }

  _ondrain () {
    if (this._pendingWrite) {
      const { cb, value } = this._pendingWrite
      this._pendingWrite = null

      this._write(value, cb)
    } else if (this._pendingEnd) {
      const cb = this._pendingEnd
      this._pendingEnd = null

      this._final(cb)
    }
  }

  _onflush () {
    while (true) {
      const value = binding.portRead(this.handle)

      if (value === null) break

      this.push(ArrayBuffer.isView(value) ? Buffer.coerce(value) : value)
    }
  }

  _onend () {
    this.push(null)
  }

  _ondestroy () {
    this._pendingDestroy(null)
  }

  _write (value, cb) {
    if (binding.portWrite(this.handle, value)) {
      cb(null)
    } else {
      this._pendingWrite = { cb, value }
    }
  }

  _final (cb) {
    if (binding.portEnd(this.handle)) {
      cb(null)
    } else {
      this._pendingEnd = cb
    }
  }

  _destroy (cb) {
    this._pendingDestroy = cb
    binding.portDestroy(this.handle)
  }
}

function mapWritable (value) {
  return typeof value === 'string' ? Buffer.from(value) : value
}
