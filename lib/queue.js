module.exports = class Queue {
  constructor(capacity = 128) {
    this._data = new Array(capacity)
    this._mask = capacity - 1
    this._length = 0
    this._read = 0
    this._write = 0
  }

  get length() {
    return this._length
  }

  get capacity() {
    return this._data.length
  }

  shift() {
    const value = this._data[this._read]

    if (value === undefined) return undefined

    this._data[this._read] = undefined
    this._read = (this._read + 1) & this._mask
    this._length--

    return value
  }

  push(value) {
    if (this._data[this._write] !== undefined) {
      throw new RangeError('Queue is full')
    }

    this._data[this._write] = value
    this._write = (this._write + 1) & this._mask
    this._length++
  }
}
