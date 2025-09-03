const test = require('brittle')
const { symbols } = require('bare-structured-clone')
const Channel = require('.')
const { Thread } = Bare

test('basic', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')

      const channel = Channel.from(handle)
      const port = channel.connect()

      for await (const data of port) {
        await port.write(data)
      }
    }
  )

  const port = channel.connect()
  const expected = ['ping', 'pong']

  for (const data of expected) {
    await port.write(data)
  }

  for await (const data of port) {
    t.alike(data, expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('read async', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')

      const channel = Channel.from(handle)
      const port = channel.connect()

      for await (const data of port) {
        await port.write(data)
      }
    }
  )

  const port = channel.connect()
  const expected = ['ping', 'pong']

  for (const data of expected) {
    await port.write(data)
  }

  while (true) {
    t.alike(await port.read(), expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('read blocking', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')

      const channel = Channel.from(handle)
      const port = channel.connect()

      for await (const data of port) {
        await port.write(data)
      }
    }
  )

  const port = channel.connect()
  const expected = ['ping', 'pong']

  for (const data of expected) {
    await port.write(data)
  }

  while (true) {
    t.alike(port.readSync(), expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('big echo', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')

      const channel = Channel.from(handle)
      const port = channel.connect()

      for await (const data of port) {
        await port.write(data)
      }
    }
  )

  const port = channel.connect()
  const sent = ['ping']

  for (let i = 0; i < 1e5; i++) sent.push(sent[0])

  const consume = t.test('consume', async (t) => {
    const read = []

    for await (const value of port) {
      read.push(value)
      if (read.length === sent.length) break
    }

    t.alike(read, sent)
  })

  for (const data of sent) {
    await port.write(data)
  }

  await consume

  await port.close()

  thread.join()
})

test('serializable interface', async (t) => {
  class Foo {
    constructor(foo) {
      this.foo = foo
    }

    [symbols.serialize]() {
      return this.foo
    }

    static [symbols.deserialize](serialized) {
      return new Foo(serialized)
    }
  }

  const channel = new Channel({ interfaces: [Foo] })

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')
      const { symbols } = require('bare-structured-clone')

      class Foo {
        constructor(foo) {
          this.foo = foo
        }

        [symbols.serialize]() {
          return this.foo
        }

        static [symbols.deserialize](serialized) {
          return new Foo(serialized)
        }
      }

      const channel = Channel.from(handle, { interfaces: [Foo] })
      const port = channel.connect()

      await port.write(await port.read())
    }
  )

  const port = channel.connect()

  const expected = new Foo('foo')

  await port.write(expected)

  t.alike(expected, await port.read())

  await port.close()

  thread.join()
})

test('detect close on teardown', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(
    __filename,
    { data: channel.handle },
    async (handle) => {
      const Channel = require('.')

      const channel = Channel.from(handle)
      const port = channel.connect()

      port.unref() // Let the thread exit
    }
  )

  const port = channel.connect()

  port.on('close', () => {
    t.pass('port closed')

    thread.join()
  })
})
