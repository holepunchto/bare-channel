const Channel = require('../..')

main()

async function main() {
  const channel = Channel.from(Bare.Thread.self.data)
  const port = channel.connect()

  await port.write('foo')

  port.unref()
}
