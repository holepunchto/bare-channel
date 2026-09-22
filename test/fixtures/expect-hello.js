const Channel = require('../..')

main()

async function main() {
  const channel = Channel.from(Bare.Thread.self.data)
  const port = channel.connect()

  port.on('close', () => {})

  if ((await port.read()) !== 'Hello') throw new Error('Failed')
}
