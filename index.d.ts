import EventEmitter, { EventMap } from 'bare-events'
import {
  SerializableConstructor,
  TransferableConstructor,
  TransferableValue
} from 'bare-structured-clone'
import {
  Duplex,
  DuplexOptions,
  Readable,
  ReadableOptions,
  Writable,
  WritableOptions
} from 'bare-stream'

interface ChannelOptions {
  handle?: SharedArrayBuffer
  interfaces?: (SerializableConstructor | TransferableConstructor)[]
}

interface Channel<T = unknown> {
  readonly handle: SharedArrayBuffer
  readonly interfaces: (SerializableConstructor | TransferableConstructor)[]

  connect(): Port<T>
}

declare class Channel {
  constructor(opts?: ChannelOptions)

  static from(handle: SharedArrayBuffer, opts?: ChannelOptions): Channel
}

interface PortEvents extends EventMap {
  end: []
  close: []
}

interface Port<T = unknown>
  extends EventEmitter<PortEvents>,
    Iterable<T>,
    AsyncIterable<T> {
  ref(): void
  unref(): void

  read(): Promise<T | null>
  readSync(): T | null

  write(value: T, opts?: { transfer: TransferableValue[] }): Promise<boolean>
  writeSync(value: T, opts?: { transfer: TransferableValue[] }): boolean

  createReadStream(opts?: ReadableOptions<Port<T>>): Readable
  createWriteStream(opts?: WritableOptions<Port<T>>): Writable
  createStream(opts?: DuplexOptions<Port<T>>): Duplex

  close(): Promise<void>
}

declare class Port<T = unknown> extends EventEmitter<PortEvents> {
  constructor(channel: Channel<T>)
}

export = Channel
