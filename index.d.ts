import EventEmitter, { EventMap } from 'bare-events'
import {
  SerializableConstructor,
  TransferableConstructor,
  TransferableValue
} from 'bare-structured-clone'

interface ChannelOptions {
  handle?: SharedArrayBuffer
  interfaces?: (SerializableConstructor | TransferableConstructor)[]
}

declare class Channel<T extends unknown = unknown> {
  constructor(opts?: ChannelOptions)

  readonly handle: SharedArrayBuffer
  readonly interfaces: (SerializableConstructor | TransferableConstructor)[]

  connect(): Port<T>

  static from(handle: SharedArrayBuffer, opts?: ChannelOptions): Channel
}

interface PortEvents extends EventMap {
  closing: []
  close: []
}

declare class Port<T extends unknown> extends EventEmitter<PortEvents> {
  constructor(channel: Channel)

  readonly closed: boolean
  readonly remoteClosed: boolean

  readonly buffered: number
  readonly drained: boolean
  readonly closing: boolean

  ref(): void
  unref(): void

  read(): Promise<T | null>
  readSync(): T | null

  write(value: T, opts?: { transfer: TransferableValue[] }): Promise<void>

  close(): Promise<void> | boolean

  [Symbol.asyncIterator](): AsyncIterator<T>
}

export = Channel
