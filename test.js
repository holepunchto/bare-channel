/* global Bare */
const test = require('brittle')
const Channel = require('.')
const { Thread } = Bare

test('basic', async (t) => {
  t.plan(2)

  const channel = new Channel()
  t.teardown(() => channel.destroy())

  const thread = new Thread(__filename, { data: channel.handle }, async (handle) => {
    const Channel = require('.')

    const channel = Channel.from(handle)
    const port = channel.connect()

    for await (const data of port) {
      port.send(data)
    }
  })

  const port = channel.connect()
  const expected = [Buffer.from('ping'), Buffer.from('pong')]

  for (const data of expected) {
    port.send(data)
  }

  for await (const data of port) {
    t.alike(data, expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('recv async', async (t) => {
  t.plan(2)

  const channel = new Channel()
  t.teardown(() => channel.destroy())

  const thread = new Thread(__filename, { data: channel.handle }, async (handle) => {
    const Channel = require('.')

    const channel = Channel.from(handle)
    const port = channel.connect()

    for await (const data of port) {
      port.send(data)
    }
  })

  const port = channel.connect()
  const expected = [Buffer.from('ping'), Buffer.from('pong')]

  for (const data of expected) {
    port.send(data)
  }

  while (true) {
    t.alike(await port.recv(), expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('recv blocking', async (t) => {
  t.plan(2)

  const channel = new Channel()
  t.teardown(() => channel.destroy())

  const thread = new Thread(__filename, { data: channel.handle }, async (handle) => {
    const Channel = require('.')

    const channel = Channel.from(handle)
    const port = channel.connect()

    for await (const data of port) {
      port.send(data)
    }
  })

  const port = channel.connect()
  const expected = [Buffer.from('ping'), Buffer.from('pong')]

  for (const data of expected) {
    port.send(data)
  }

  while (true) {
    t.alike(port.recv(true), expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})
