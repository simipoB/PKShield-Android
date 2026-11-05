import cv2, sys, os, numpy as np
cap = cv2.VideoCapture(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
fr = []
while True:
    ok, f = cap.read()
    if not ok: break
    fr.append(cv2.resize(f, (480, 222)).astype(np.int16))
blk = lambda x: cv2.resize(np.abs(x).astype(np.float32).mean(axis=2), (30, 14), interpolation=cv2.INTER_AREA)
n = 0
for i in range(1, len(fr) - 1):
    a, b, c = blk(fr[i] - fr[i-1]), blk(fr[i] - fr[i+1]), blk(fr[i-1] - fr[i+1])
    m = np.minimum(a, b); score = m - 2 * c   # changed against both neighbours, while the neighbours agree
    k = np.unravel_index(np.argmax(score), score.shape)
    if score[k] > 25:
        n += 1
        if n <= 12:
            print('frame %d: block %s jump %.0f (neighbours %.0f)' % (i, k, m[k], c[k]))
            cv2.imwrite(os.path.join(out, 'b%04d.png' % i), np.hstack([fr[i-1], fr[i], fr[i+1]]).astype(np.uint8))
print('local flash frames', n, 'of', len(fr))
