#!/bin/bash  -x

test -f src/qt/maza.cpp || { echo "Your current directory is $(pwd)" ;echo  "You should be in the toplevel directory of the project to demazafy it" ; exit 1; }

if [ "$0" != "/tmp/demazafy/demazafy.sh" ] ; then
   mkdir /tmp/demazafy || { echo "Could not create /tmp/demazafy" ; exit 1; }
   cp ./demazafy.sh /tmp/demazafy || { echo "Could not move script to /tmp/demazafy" ; exit 1; }
   exec /tmp/demazafy/demazafy.sh
fi

find . -type d |egrep -v '\./\.git' > /tmp/demazafy/de_mazafy.dirs
while read line ; do
  pushd $line
  find ./ -type f -not -name '*.png' -not -name '*.raw' -not -name '*.ico' -not -name '*.bmp' -not -name demazafy.sh -not -name '*.icns'  -maxdepth 1 > /tmp/demazafy/filelist
  while read filename ; do
    echo "change $(pwd)/$filename"
    sed -i.bkp -e 's/mazacoin/mazacoin/g' -e 's/Maza/Maza/g' -e 's/MAZA/MAZAg' -e 's/maza/maza/g' $filename
  done < /tmp/demazafy/filelist
  rm /tmp/demazafy/filelist
  popd
done < /tmp/demazafy/de_mazafy.dirs
cat << EOF > /tmp/demazafy/de_mazafy.files
./contrib/init/mazad.conf
./contrib/init/mazad.init
./contrib/init/mazad.openrc
./contrib/init/mazad.openrcconf
./contrib/init/mazad.service
./contrib/init/org.maza.mazad.plist
./contrib/maza-cli.bash-completion
./contrib/maza-qt.pro
./contrib/maza-tx.bash-completion
./contrib/mazad.bash-completion
./doc/man/maza-cli.1
./doc/man/maza-qt.1
./doc/man/maza-tx.1
./doc/man/mazad.1
./doc/mazacoin-developer-notes.md
./libmazaconsensus.pc.in
./src/bench/bench_maza.cpp
./src/maza-cli-res.rc
./src/maza-cli.cpp
./src/maza-tx-res.rc
./src/maza-tx.cpp
./src/mazad-res.rc
./src/mazad.cpp
./src/qt/locale/maza_bg.ts
./src/qt/locale/maza_de.ts
./src/qt/locale/maza_en.ts
./src/qt/locale/maza_es.ts
./src/qt/locale/maza_fi.ts
./src/qt/locale/maza_fr.ts
./src/qt/locale/maza_it.ts
./src/qt/locale/maza_ja.ts
./src/qt/locale/maza_nl.ts
./src/qt/locale/maza_pl.ts
./src/qt/locale/maza_pt.ts
./src/qt/locale/maza_pt_BR.ts
./src/qt/locale/maza_ru.ts
./src/qt/locale/maza_sk.ts
./src/qt/locale/maza_sv.ts
./src/qt/locale/maza_vi.ts
./src/qt/locale/maza_zh_CN.ts
./src/qt/locale/maza_zh_TW.ts
./src/qt/maza.cpp
./src/qt/maza.qrc
./src/qt/maza_locale.qrc
./src/qt/mazastrings.cpp
./src/qt/res/images/crownium/maza_logo_horizontal.png
./src/qt/res/images/drkblue/maza_logo_horizontal.png
./src/qt/res/images/light/maza_logo_horizontal.png
./src/qt/res/images/light-retro/maza_logo_horizontal.png
./src/qt/res/images/trad/maza_logo_horizontal.png
./src/qt/res/maza-qt-res.rc
./src/script/mazaconsensus.cpp
./src/script/mazaconsensus.h
./src/test/test_maza.cpp
./src/test/test_maza.h
EOF
while read line ; do 
  fname=$(echo $line | sed 's/maza/maza/g')
  mv $line $fname 
done < /tmp/demazafy/de_mazafy.files


# exceptions 

# .travis.yml
# /bin/XXXMAZA should not change to /bin/maza

#diff -r ./src/clientversion.cpp ../XXXMAZA_untouched/src/clientversion.cpp
#16c16
#< const std::string CLIENT_NAME("Buffalo");
#---
#> const std::string CLIENT_NAME("Maza Network");
#diff -r ./src/clientversion.h ../XXXMAZA_untouched/src/clientversion.h
#41c41
#< #define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " The Maza Network Developers"
#---
#> #define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " The Bitcoin Core Developers, 2014-" STRINGIZE(COPYRIGHT_YEAR) " " COPYRIGHT_HOLDERS_FINAL
#
#diff -r ./src/validation.cpp ../XXXMAZA_untouched/src/validation.cpp
#106c106
#< const std::string strMessageMagic = "Maza Signed Message:\n";
#---
#> const std::string strMessageMagic = "DarkCoin Signed Message:\n";

#diff -r ./src/qt/locale/XXXMAZA_sk.ts ../XXXMAZA_untouched/src/qt/locale/XXXMAZA_sk.ts
#2237c2237
#<         <translation>VyXXXMAZAť konzolu</translation>
#---
#>         <translation>Vymazať konzolu</translation>
#2869c2869
#<         <translation>ZXXXMAZAť &amp;všetko</translation>
#---
#>         <translation>Zmazať &amp;všetko</translation>
#3185c3185
#<         <translation>ZXXXMAZAť &amp;všetko</translation>
#---
#>         <translation>Zmazať &amp;všetko</translation>
#4046c4046
#<         <translation>VyXXXMAZAť všetky transakcie z peňaženky a pri spustení znova získať z reťazca blokov iba tie získané pomocou -rescan</translation>
#---
#>         <translation>Vymazať všetky transakcie z peňaženky a pri spustení znova získať z reťazca blokov iba tie získané pomocou -rescan</translation>
#4078c4078
#<         <translation>Uistite sa, že máte´Vašu peňaženku zašifrovanú a zXXXMAZAné všetky nezašifrované zálohy potom, ako overíte, že peňaženka funguje! </translation>
#---
#>         <translation>Uistite sa, že máte´Vašu peňaženku zašifrovanú a zmazané všetky nezašifrované zálohy potom, ako overíte, že peňaženka funguje! </translation>
#4702c4702
#<         <translation>Nepodarilo sa vyXXXMAZAť zálohu, chyba: %s</translation>
#---
#>         <translation>Nepodarilo sa vymazať zálohu, chyba: %s</translation>
#5010c5010
#<         <translation>Obmedziť nároky na úložný priestor prerezáváním (XXXMAZAním) starých blokov. Táto volba tiež umožní použiť RPC volanie pruneblockchain na zXXXMAZAnie konkrétnych blokov a ďalej automatické prerezávanie starých blokov, ak je zadána cieľová velikosť súborov z blokmi v MiB. Tento režim nie je zlúčiteľný s -txindex ani -rescan. Upozornenie: opätovná zmena tohoto nastavenia bude vyžadovať nové stiahnutie celého reťazca blokov. (predvolené: 0 = bloky neprerezávať, 1 = povoliť ručné prerezávanie cez RPC, &gt;%u = automatické prerezávanie blokov tak, aby bola udržaná cieľová velikosť súborov s blokmi v MiB)</translation>
#---
#>         <translation>Obmedziť nároky na úložný priestor prerezáváním (mazaním) starých blokov. Táto volba tiež umožní použiť RPC volanie pruneblockchain na zmazanie konkrétnych blokov a ďalej automatické prerezávanie starých blokov, ak je zadána cieľová velikosť súborov z blokmi v MiB. Tento režim nie je zlúčiteľný s -txindex ani -rescan. Upozornenie: opätovná zmena tohoto nastavenia bude vyžadovať nové stiahnutie celého reťazca blokov. (predvolené: 0 = bloky neprerezávať, 1 = povoliť ručné prerezávanie cez RPC, &gt;%u = automatické prerezávanie blokov tak, aby bola udržaná cieľová velikosť súborov s blokmi v MiB)</translation>
#5342c5342
#<         <translation>VyXXXMAZAť všetky transakcie z peňaženky...</translation>
#---
#>         <translation>Vymazať všetky transakcie z peňaženky...</translation>
#5345c5345
#< </TS>
#---
#> </TS>
#\ No newline at end of file


# clean up 
# in case we modify above and self-hose our script copy the original back from /tmp
cp /tmp/demazafy/demazafy.sh .
rm -rf /tmp/demazafy
# remove files named with .![PID] - binary files we tried to change with sed
find ./ -name '.\!*' | xargs rm 
# remove sed backups 
find ./ -name '*.bkp' | xargs rm
