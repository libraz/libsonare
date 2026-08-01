import { Audio } from '@libraz/libsonare-native';

const [path] = process.argv.slice(2);
if (!path) {
  console.error(`usage: ${process.argv[1]} INPUT_AUDIO`);
  process.exit(2);
}

const audio = Audio.fromFile(path);
try {
  console.log(JSON.stringify(audio.analyze(), null, 2));
} finally {
  audio.destroy();
}
