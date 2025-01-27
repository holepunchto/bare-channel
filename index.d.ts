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
  closing: []
  close: []
}

interface Port<T = unknown> extends EventEmitter<PortEvents>, AsyncIterable<T> {
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

  close(): Promise<void>
}

declare class Port<T = unknown> extends EventEmitter<PortEvents> {
  constructor(channel: Channel<T>)
}

export = Channel
