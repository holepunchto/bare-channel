const Channel = require('../..')

main()

async function main() {
  const channel = Channel.from(Bare.Thread.self.data)
  const port = channel.connect()

  for await (const data of port) {
    await port.write(data)
  }
}
