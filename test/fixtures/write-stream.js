const Channel = require('../..')

const { handle, count } = Bare.Thread.self.data

const channel = Channel.from(handle)
const port = channel.connect()
const stream = port.createWriteStream()

for (let i = 0; i < count; i++) {
  stream.write(Buffer.from(`${i}`))
}

stream.end()
