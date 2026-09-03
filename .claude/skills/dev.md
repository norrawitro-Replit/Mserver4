---
name: dev
description: Start development server
---

# Development Server

## Start Dev Server
```bash
npm run dev
```

## Expected Output
- [TODO] Server running on `localhost:PORT`
- [TODO] Hot reload enabled
- [TODO] No errors in console

## Environment Variables
```bash
# Create .env.local
NODE_ENV=development
[TODO] Add more env vars
```

## Common Issues
### Port Already in Use
```bash
# Kill process on port 3000
lsof -ti:3000 | xargs kill -9
```

### Dependencies Missing
```bash
npm install
```

## Debug Mode
```bash
DEBUG=* npm run dev
```

## Testing While Developing
- Keep dev server running
- Run `/test` in another terminal
- Or use watch mode: `npm run test:watch`

## Notes
- [TODO] Document API endpoints
- [TODO] Document database setup
