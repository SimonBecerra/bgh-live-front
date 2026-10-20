#!/usr/bin/env python3
"""Turns The New Believers Hymn Book text file into data/new-believers.json for the remote's Hymns mode.

    python3 tools/new-believers-to-json.py "The New Believers Hymn Book.txt"

The text file: "Hymn N: Title", then optional Words:/Music:/Tune:/Meter: lines, then numbered verses
(the number on its own line) and "Chorus" blocks, separated by blank lines.
Each hymn in the JSON: {"n": number, "t": title, "words"/"music"/"tune"/"meter" when given,
"b": [{"k": "v" for a verse or "c" for a chorus, "n": the verse number, "l": the lines}]}.
"""
import json,os,re,sys,collections
F=sys.argv[1]
lines=open(F,encoding='utf-8').read().replace('\r\n','\n').split('\n')
H=[];cur=None;blk=None;stray=[];meta_after=[]
for i,raw in enumerate(lines,1):
    l=raw.rstrip()
    m=re.match(r'^Hymn (\d+):\s*(.*)$',l)
    if m:
        cur={'n':int(m.group(1)),'t':m.group(2).strip(),'b':[]};H.append(cur);blk=None;continue
    if cur is None:
        if l.strip(): stray.append((i,l))
        continue
    s=l.strip()
    if not s:
        blk=None if blk is None or blk['l'] else blk;continue
    mm=re.match(r'^(Words|Music|Tune|Meter):\s*(.*)$',s)
    if mm and blk is None:
        if cur['b']: meta_after.append((cur['n'],s))
        cur[mm.group(1).lower()]=mm.group(2).strip();continue
    if re.fullmatch(r'\d+',s):
        blk={'k':'v','n':s,'l':[]};cur['b'].append(blk);continue
    if re.fullmatch(r'(?i)(chorus|refrain):?',s):
        blk={'k':'c','n':'','l':[]};cur['b'].append(blk);continue
    if blk is None:
        # a lyric line after a blank line with no new number: continues the previous block
        if cur['b']: blk=cur['b'][-1]
        else: blk={'k':'v','n':'1','l':[]};cur['b'].append(blk);stray.append((i,'(no number) '+s))
    blk['l'].append(s)
for h in H:
    for b in h['b']: b['l']='\n'.join(b['l'])
    h['b']=[b for b in h['b'] if b['l']]
nums=[h['n'] for h in H]
print('hymns',len(H),'unique',len(set(nums)),'range',min(nums),max(nums),'missing',sorted(set(range(1,701))-set(nums))[:10])
print('stray',stray[:10]);print('meta after verses',meta_after[:5])
pat=collections.Counter(''.join(b['k'] for b in h['b']).replace('vc','VC') for h in H)
cpat=collections.Counter()
for h in H:
    s=''.join(b['k'] for b in h['b'])
    if 'c' in s: cpat[s]+=1
print('with chorus',sum(cpat.values()),cpat.most_common(8))
print('empty hymns',[h['n'] for h in H if not h['b']])
# verse numbering sanity
bad=[h['n'] for h in H if [b['n'] for b in h['b'] if b['k']=='v']!=[str(i+1) for i in range(sum(1 for b in h['b'] if b['k']=='v'))]]
print('verse numbers not 1..n:',len(bad),bad[:10])
OUT=sys.argv[2] if len(sys.argv)>2 else os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','data','new-believers.json')
json.dump(H,open(OUT,'w',encoding='utf-8'),ensure_ascii=False,separators=(',',':'))
print('wrote',OUT,os.path.getsize(OUT),'bytes')
