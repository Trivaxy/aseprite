import type * as Party from "partykit/server";

export default class RelayServer implements Party.Server {
    private hostId: string | null = null;

    private static readonly kEnvelopeMagic = 0xff;
    private static readonly kEnvelopeGuestToHost = 1;
    private static readonly kEnvelopeHostToGuest = 2;

    constructor(readonly room: Party.Room) {
        console.log(`[Room ${room.id}] Server created`);
    }

    private encodeEnvelope(kind: number, id: string, payload: ArrayBuffer): ArrayBuffer {
        const idBytes = new TextEncoder().encode(id);
        const out = new Uint8Array(2 + 4 + idBytes.length + payload.byteLength);
        out[0] = RelayServer.kEnvelopeMagic;
        out[1] = kind;

        const dv = new DataView(out.buffer);
        dv.setUint32(2, idBytes.length, true);

        out.set(idBytes, 6);
        out.set(new Uint8Array(payload), 6 + idBytes.length);
        return out.buffer;
    }

    private decodeEnvelope(payload: ArrayBuffer): { kind: number; id: string; payload: ArrayBuffer } | null {
        const bytes = new Uint8Array(payload);
        if (bytes.byteLength < 2 + 4)
            return null;
        if (bytes[0] !== RelayServer.kEnvelopeMagic)
            return null;

        const kind = bytes[1];
        const dv = new DataView(payload);
        const idLen = dv.getUint32(2, true);
        if (bytes.byteLength < 2 + 4 + idLen)
            return null;

        const idBytes = bytes.slice(6, 6 + idLen);
        const id = new TextDecoder().decode(idBytes);
        const innerPayload = bytes.slice(6 + idLen).buffer;
        return { kind, id, payload: innerPayload };
    }

    onConnect(connection: Party.Connection, ctx: Party.ConnectionContext) {
        const url = new URL(ctx.request.url);
        const isHost = url.searchParams.get("host") === "true";

        console.log(`[Room ${this.room.id}] Connection: ${connection.id}, isHost: ${isHost}`);

        if (isHost) {
            if (this.hostId !== null) {
                console.log(`[Room ${this.room.id}] Rejecting duplicate host`);
                connection.send(
                    JSON.stringify({ type: "error", message: "Room already exists" })
                );
                connection.close();
                return;
            }
            this.hostId = connection.id;
            console.log(`[Room ${this.room.id}] Host registered: ${connection.id}`);
            connection.send(JSON.stringify({ type: "host_ok" }));
        } else {
            if (this.hostId === null) {
                console.log(`[Room ${this.room.id}] Rejecting guest - no host`);
                connection.send(
                    JSON.stringify({ type: "error", message: "Room not found" })
                );
                connection.close();
                return;
            }
            console.log(`[Room ${this.room.id}] Guest connected: ${connection.id}`);
            connection.send(JSON.stringify({ type: "guest_ok" }));
        }
    }

    onMessage(message: string | ArrayBuffer, sender: Party.Connection) {
        const msgType = typeof message === 'string' ? 'text' : 'binary';
        const msgSize = typeof message === 'string' ? message.length : message.byteLength;

        console.log(`[Room ${this.room.id}] Message from ${sender.id}: ${msgType}, size=${msgSize}`);

        if (sender.id === this.hostId) {
            // Host -> targeted send (envelope) or broadcast (raw)
            if (message instanceof ArrayBuffer) {
                const env = this.decodeEnvelope(message);
                if (env && env.kind === RelayServer.kEnvelopeHostToGuest) {
                    const target = this.room.getConnection(env.id);
                    if (target) {
                        target.send(env.payload);
                    }
                    return;
                }
            }

            const connections = [...this.room.getConnections()];
            const guestCount = connections.filter(c => c.id !== this.hostId).length;
            console.log(`[Room ${this.room.id}] Host -> broadcasting to ${guestCount} guests`);
            this.room.broadcast(message, [sender.id]);
        } else {
            // Guest -> send only to host
            console.log(`[Room ${this.room.id}] Guest ${sender.id} -> forwarding to host ${this.hostId}`);
            if (this.hostId) {
                const host = this.room.getConnection(this.hostId);
                if (host) {
                    console.log(`[Room ${this.room.id}] Forwarding to host succeeded`);
                    if (message instanceof ArrayBuffer) {
                        host.send(this.encodeEnvelope(RelayServer.kEnvelopeGuestToHost, sender.id, message));
                    } else {
                        host.send(message);
                    }
                } else {
                    console.log(`[Room ${this.room.id}] ERROR: Host connection not found!`);
                }
            } else {
                console.log(`[Room ${this.room.id}] ERROR: No hostId set!`);
            }
        }
    }

    onClose(connection: Party.Connection) {
        console.log(`[Room ${this.room.id}] Connection closed: ${connection.id}`);
        if (connection.id === this.hostId) {
            console.log(`[Room ${this.room.id}] Host disconnected, closing all guests`);
            this.hostId = null;
            for (const conn of this.room.getConnections()) {
                conn.send(JSON.stringify({ type: "host_disconnected" }));
                conn.close();
            }
        }
    }
}
