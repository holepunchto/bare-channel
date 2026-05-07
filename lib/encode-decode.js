const structuredClone = require('bare-structured-clone')

exports.encode = function encode(channel, value, opts) {
  const serialized = structuredClone.serializeWithTransfer(value, opts.transfer, channel.interfaces)

  const state = { start: 0, end: 0, buffer: null }

  structuredClone.preencode(state, serialized)

  const data = new ArrayBuffer(state.end)

  state.buffer = Buffer.from(data)

  structuredClone.encode(state, serialized)

  return data
}

exports.decode = function decode(channel, data) {
  const state = {
    start: 0,
    end: data.byteLength,
    buffer: Buffer.from(data)
  }

  return structuredClone.deserializeWithTransfer(structuredClone.decode(state), channel.interfaces)
}
