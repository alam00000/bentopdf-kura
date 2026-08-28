.PHONY: build wasm npm-pack site-sync check lint smoke version-check shell-check yaml-check docs docs-dev fuzz clean

build:
	cmake -S pdfa-engine -B pdfa-engine/build -DCMAKE_BUILD_TYPE=Release
	cmake --build pdfa-engine/build -j

wasm:
	pdfa-engine/scripts/build-wasm.sh

npm-pack:
	scripts/pack-wasm-npm.sh

site-sync:
	cp pdfa-engine/build-wasm/wasm/kura.js pdfa-engine/build-wasm/wasm/kura.wasm site/

check: shell-check yaml-check version-check lint smoke

lint:
	npm run --silent lint

smoke:
	@test -f packages/npm/kura-pdf/engine/kura.wasm || scripts/pack-wasm-npm.sh site >/dev/null
	node scripts/smoke.mjs

version-check:
	@node -e "const fs=require('fs');const h=fs.readFileSync('pdfa-engine/core/include/kura/kura.h','utf8').match(/KURA_VERSION \"(.*)\"/)[1];const e=fs.readFileSync('pdfa-engine/core/include/pdfa/pdfa.hh','utf8').match(/kEngineVersion = \"(.*)\"/)[1];const p=require('./package.json').version;const n=require('./packages/npm/kura-pdf/package.json').version;const all=[h,e,p,n];if(new Set(all).size!==1){console.error('version drift: kura.h '+h+', pdfa.hh '+e+', package.json '+p+', kura-pdf '+n);process.exit(1)}console.log('version '+h)"

shell-check:
	@for f in scripts/*.sh pdfa-engine/scripts/*.sh pdfa-engine/scripts/pdfium/*.sh .clusterfuzzlite/build.sh; do bash -n "$$f" || exit 1; done
	@echo "shell syntax ok"

yaml-check:
	@node -e "const fs=require('fs');const p=require('path');const dirs=['.github/workflows','.github/ISSUE_TEMPLATE','.github','.clusterfuzzlite'];let n=0;for(const d of dirs){for(const f of fs.readdirSync(d)){if(!/\.ya?ml$$/.test(f))continue;const t=fs.readFileSync(p.join(d,f),'utf8');if(/\t/.test(t)){console.error(p.join(d,f)+': tab character');process.exit(1)}n++}}console.log(n+' yaml files scanned')"

docs:
	npm run --silent docs:build

docs-dev:
	npm run --silent docs:dev

fuzz:
	cmake -S pdfa-engine -B pdfa-engine/build-fuzz -DPDFA_BUILD_FUZZ=ON -DPDFA_FUZZ_LIBFUZZER=ON -DPDFA_BUILD_CLI=OFF -DPDFA_BUILD_SDK=OFF -DPDFA_WITH_PDFIUM=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build pdfa-engine/build-fuzz --target fuzz_convert -j
	python3 pdfa-engine/fuzz/gen_seeds.py pdfa-engine/build-fuzz/seeds
	pdfa-engine/build-fuzz/fuzz/fuzz_convert pdfa-engine/build-fuzz/seeds -max_total_time=60 -rss_limit_mb=4096

clean:
	rm -rf pdfa-engine/build pdfa-engine/build-wasm pdfa-engine/build-fuzz packages/npm/kura-pdf/engine docs/.vitepress/dist docs/.vitepress/cache
