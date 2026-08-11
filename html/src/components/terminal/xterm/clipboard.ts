import { ClipboardProtocol } from './clipboard-protocol.generated';

const supportedImageTypes = new Set<string>(ClipboardProtocol.supportedImageTypes);

export function findClipboardImage(items?: DataTransferItemList): File | undefined {
    if (!items) return undefined;
    for (let index = 0; index < items.length; index++) {
        const item = items[index];
        if (item.kind !== 'file' || !item.type.startsWith('image/')) continue;
        const file = item.getAsFile();
        if (file) return file;
    }
    return undefined;
}

export function validateClipboardImage(file: File, maxSize: number): string | undefined {
    if (!supportedImageTypes.has(file.type)) return `Unsupported clipboard image type: ${file.type || 'unknown'}`;
    if (file.size === 0) return 'The clipboard image is empty';
    if (file.size > maxSize) return `The clipboard image exceeds the ${formatBytes(maxSize)} limit`;
    return undefined;
}

export function encodeCommand(command: string, data?: string | Uint8Array): Uint8Array {
    const encoded = typeof data === 'string' ? new TextEncoder().encode(data) : data;
    const payload = new Uint8Array((encoded?.length || 0) + 1);
    payload[0] = command.charCodeAt(0);
    if (encoded) payload.set(encoded, 1);
    return payload;
}

function formatBytes(bytes: number): string {
    if (bytes < 1024 * 1024) return `${Math.ceil(bytes / 1024)} KiB`;
    return `${Math.ceil(bytes / (1024 * 1024))} MiB`;
}
