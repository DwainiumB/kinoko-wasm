import sys, os
sys.path.insert(0, r'C:\Users\dwain\Kinoko\tools')
import brres_to_glb as b
CI=['CPREV','APREV','C0','A0','C1','A1','C2','A2','TEXC','TEXA','RASC','RASA','ONE','HALF','KONST','ZERO']
AI=['APREV','A0','A1','A2','TEXA','RASA','KONST','ZERO']
f=b.u8_files(b.yaz0_decompress(open(r'C:\Users\dwain\MKWii\Race\Course\koopa_course.szs','rb').read()))
files=sys.argv[1:] or ['course_model.brres']
for fn in files:
    r=b.Reader(f[fn]); c=b.brres_contents(r)
    print('=====',fn,{k:[n for n,_ in v] for k,v in c.items() if k!='Textures(NW4R)'})
    for name,off in c.get('3DModels(NW4R)',[]):
        m=b.Mdl0(r,off,name)
        print('-- model',name)
        for i,mt in b.parse_materials(m).items():
            sh=mt.off+r.s32(mt.off+0x28); sd=r.d[sh:sh+r.u32(sh)]
            bw=dict(b._bp_writes(r.d[mt.off:mt.off+r.u32(mt.off)]))
            st=[]
            for reg,v in b._bp_writes(sd):
                if 0xC0<=reg<=0xDF and reg%2==0: st.append('C%d(%s,%s,%s,%s)x%s'%((reg-0xC0)//2,CI[v>>12&15],CI[v>>8&15],CI[v>>4&15],CI[v&15],[1,2,4,.5][v>>20&3]))
            ind=[hex(v) for g,v in b._bp_writes(sd) if g==0x27]
            print(mt.name,[t['name'] for t in mt.textures],'xlu' if mt.xlu else '','cull',mt.cull,'blend=%06X'%bw.get(0x41,0),' '.join(st),'ind' if ind and ind[-1]!='0xffffff' else '', 'shadow',mt.shadow_color,mt.shadow_mode,'regular',mt.regular,'ac',mt.alpha_compare)
