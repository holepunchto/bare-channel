const test = require('brittle')
const Thread = require('bare-thread')
const { symbols } = require('bare-structured-clone')
const Channel = require('.')

test('basic', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/echo'), {
    data: channel.handle
  })

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

  const thread = new Thread(require.resolve('./test/fixtures/echo'), {
    data: channel.handle
  })

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

  const thread = new Thread(require.resolve('./test/fixtures/echo'), {
    data: channel.handle
  })

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

test('write blocking', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/echo'), {
    data: channel.handle
  })

  const port = channel.connect()
  const expected = ['ping', 'pong']

  for (const data of expected) {
    port.writeSync(data)
  }

  while (true) {
    t.alike(await port.read(), expected.shift())
    if (expected.length === 0) break
  }

  await port.close()

  thread.join()
})

test('big echo', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/echo'), {
    data: channel.handle
  })

  const port = channel.connect()
  const sent = []

  for (let i = 0; i < 1e5; i++) sent.push(i)

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

  const thread = new Thread(require.resolve('./test/fixtures/serializable-interface'), {
    data: channel.handle
  })

  const port = channel.connect()

  const expected = new Foo('foo')

  await port.write(expected)

  t.alike(expected, await port.read())

  await port.close()

  thread.join()
})

test('unref', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/unref'), {
    data: channel.handle
  })

  const port = channel.connect()

  port.on('close', () => {
    t.pass('port closed')

    thread.join()
  })
})

test('close primary before unref', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/unref'), {
    data: channel.handle
  })

  const port = channel.connect()

  port
    .on('close', () => {
      t.pass('port closed')

      thread.join()
    })
    .close()
})

test('close secondary after unref', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/unref-close'), {
    data: channel.handle
  })

  const port = channel.connect()

  port.on('close', () => {
    t.pass('port closed')

    thread.join()
  })
})

test('write after remote end', async (t) => {
  t.plan(2)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/close'), {
    data: channel.handle
  })

  const port = channel.connect()

  port.on('close', async () => {
    t.pass('port closed')

    t.is(await port.write('ping'), false)

    thread.join()
  })
})

test('close immediately after write', async (t) => {
  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/expect-hello'), {
    data: channel.handle
  })

  const port = channel.connect()

  await port.write('Hello')
  await port.close()

  thread.join()
})

test('read stream', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/write-many'), {
    data: { handle: channel.handle, count: 1e3 }
  })

  const port = channel.connect()
  const stream = port.createReadStream()
  const received = []

  stream
    .on('data', (value) => received.push(value))
    .on('close', () => {
      t.alike(
        received,
        new Array(1e3).fill(0).map((_, i) => i)
      )

      thread.join()
    })
})

test('write stream', async (t) => {
  t.plan(1)

  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/write-stream'), {
    data: { handle: channel.handle, count: 1e3 }
  })

  const port = channel.connect()
  const received = []

  for await (const i of port) {
    received.push(i.toString())
  }

  t.alike(
    received,
    new Array(1e3).fill(0).map((_, i) => `${i}`)
  )

  thread.join()
})

test('both sides close', async (t) => {
  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/close'), {
    data: channel.handle
  })

  const port = channel.connect()
  await port.close()

  thread.join()
})

test('both sides unref', async (t) => {
  const channel = new Channel()

  const a = new Thread(require.resolve('./test/fixtures/unref'), {
    data: channel.handle
  })

  const b = new Thread(require.resolve('./test/fixtures/unref-write'), {
    data: channel.handle
  })

  a.join()
  b.join()
})

test('both sides unref in same thread', async (t) => {
  const channel = new Channel()

  const thread = new Thread(require.resolve('./test/fixtures/unref-both'), {
    data: channel.handle
  })

  thread.join()
})
