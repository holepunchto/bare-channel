const Channel = require('../..')

main()

async function main() {
  const { handle, count } = Bare.Thread.self.data

  const channel = Channel.from(handle)
  const port = channel.connect()

  for (let i = 0; i < count; i++) {
    await port.write(i)
  }

  await port.close()
}
