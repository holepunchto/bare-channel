const { symbols } = require('bare-structured-clone')
const Channel = require('../..')

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

main()

async function main() {
  const channel = Channel.from(Bare.Thread.self.data, { interfaces: [Foo] })
  const port = channel.connect()

  await port.write(await port.read())
}
