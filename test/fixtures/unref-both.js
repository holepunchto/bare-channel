const Channel = require('../..')

const channel = Channel.from(Bare.Thread.self.data)
const a = channel.connect()
const b = channel.connect()

a.unref()
b.unref()
