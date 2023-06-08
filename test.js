const Thread = require('thread')
const test = require('brittle')
const Channel = require('.')

test('basic', (t) => {
  t.plan(2)

  const channel = new Channel()
  t.teardown(() => channel.destroy())

  const thread = new Thread(__filename, { data: channel.handle }, (handle) => {
    const Channel = require('.')

    const channel = Channel.from(handle)

    const port = channel.connect()
    port
      .on('data', (data) => port.end(data))
  })

  const port = channel.connect()
  port
    .on('data', (data) => t.alike(data, Buffer.from('ping')))
    .on('close', () => {
      thread.join()

      t.pass('thread joined')
    })
    .end('ping')
})
