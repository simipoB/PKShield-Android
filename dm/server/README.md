# Room server: local play over the internet

Pokémon Sword and Shield's local play (link battles, trades, Max Raids) normally needs consoles in range. On Wi-Fi, phones in the app find each other on their own. For players in different places, a **room server** gives everyone who enters the same room code a shared virtual local network, and the game plays as if they were in range.

The server is `room_server.py`: standard Python 3, no packages. It only relays the game's local-play packets between the members of a room. It holds no accounts, saves or game files, and it never connects to Nintendo.

## Host your own on Railway

1. Make a Railway project from this folder (`dm/server`). `railway.json` makes Railway start it with `python3 room_server.py`.
2. In the service's **Settings → Networking**, add a **TCP Proxy** for port **11450**. Railway shows an address like `something.proxy.rlwy.net:12345`.
3. In the game, open **☰ → Online room**, enter that address and a room code, and share both with your friends. Everyone restarts the game, then uses the game's own **Local** link options.

Railway charges by usage, and a room server needs very little. You can stop the service whenever you're not playing.

## Or run it on any PC

```
python3 room_server.py          # port 11450, or: PORT=12345 python3 room_server.py
```

Players then enter `<that PC's address>:11450`. Over the internet, forward that TCP port on the router.

## Notes

- A room holds up to 8 players, like the game's local play.
- Mods are off for local play. With any mod on, the game reports local play as unavailable, so everyone plays the unmodified game.
- Sword and Shield players can share a room, as on a Switch.
