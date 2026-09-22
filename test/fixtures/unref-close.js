const Channel = require('../..')

const channel = Channel.from(Bare.Thread.self.data)
const port = channel.connect()

port.unref()
port.close()
