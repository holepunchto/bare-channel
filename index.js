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

    this.handle = binding.portInit(channel.handle, this,
      this._onread,
      this._onwrite,
      this._onend,
      this._ondestroy
    )

    this._destroyCallback = null
  }

  _onread () {
  }

  _onwrite () {
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
    this._destroyCallback(null)
  }

  _write (value, cb) {
    if (binding.portWrite(this.handle, value)) {
      cb(null)
    }
  }

  _final (cb) {
    if (binding.portEnd(this.handle)) {
      cb(null)
    }
  }

  _destroy (cb) {
    this._destroyCallback = cb
    binding.portDestroy(this.handle)
  }
}

function mapWritable (value) {
  return typeof value === 'string' ? Buffer.from(value) : value
}
