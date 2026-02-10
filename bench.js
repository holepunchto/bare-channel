const Channel = require('.')
const { Thread } = Bare

const channel = new Channel()

const thread = new Thread(__filename, { data: channel.handle }, async (handle) => {
  const Channel = require('.')

  const channel = Channel.from(handle)
  const port = channel.connect()

  for await (const data of port) {
    await port.write(data)
  }
})

const port = channel.connect()

const ops = 2e6

const start = Date.now()

read()
write()

async function read() {
  for (let i = 0; i < ops; i++) {
    await port.read()
  }

  const elapsed = Date.now() - start

  console.log(((ops * 2) / elapsed) * 1000, 'messages/s')
}

async function write() {
  for (let i = 0; i < ops; i++) {
    await port.write(i)
  }

  await port.close()

  thread.join()
}
