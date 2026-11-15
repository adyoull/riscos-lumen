REM A stand-in for RiscOSM: GeoData messages to and from Lumen, logged.
ON ERROR PROCerr : END
DIM b% 256, q% 32, ml% 40
log$="IXFS:$.home.claude.rol.out.maplog"
f%=OPENOUT(log$) : CLOSE#f%
$q%="Derwent"
ml%!0=&546D4 : ml%!4=&546D6 : ml%!8=&546D9 : ml%!12=3 : ml%!16=&546D0 : ml%!20=0
SYS "Wimp_Initialise",380,&4B534154,"RiscOSM",ml% TO ver%,me%
PROClog("started "+STR$~me%)
searched%=0 : fetched%=0 : shown%=0
REPEAT
  SYS "Wimp_Poll",&31,b% TO r%
  IF r%=17 OR r%=18 THEN PROCmsg
UNTIL FALSE
END

DEF PROCmsg
LOCAL m%,t%
m%=b%!16 : t%=b%!4
CASE m% OF
WHEN &546D4
  PROClog("DatasetDetails id "+STR$(b%!20)+" box "+STR$(b%!24)+" "+STR$(b%!28)+" "+STR$(b%!32)+" "+STR$(b%!36)+" types &"+STR$~(b%!40)+" other &"+STR$~(b%!44)+" title "+FNs(b%+48))
  IF b%!40<>0 AND searched%=0 THEN PROCfetch(t%,b%!24-10000,b%!28-10000,b%!32+10000,b%!36+10000)
WHEN &546D0
  PROClog("OpenMap flags &"+STR$~(b%!20)+" "+STR$(b%!24)+" "+STR$(b%!28)+" "+STR$(b%!32)+" "+STR$(b%!36)+" title "+FNs(b%+60))
  b%!12=b%!8 : b%!16=&546D2 : b%!20=9 : b%!0=84
  SYS "Wimp_SendMessage",17,b%,t%
  PROClog("sent MapInfo map 9")
WHEN &546D9
  PROClog("Status "+STR$(b%!20)+" "+STR$(b%!24)+" "+FNs(b%+28)+" your_ref "+STR$~(b%!12))
WHEN &546D6
  PROClog("TransmitData map "+STR$(b%!20)+" ds "+STR$(b%!24)+" obj "+STR$(b%!28)+" type &"+STR$~(b%!32)+" flags &"+STR$~(b%!36)+" lon "+STR$(b%!40)+" lat "+STR$(b%!44)+" alt "+STR$(b%!48)+" bearing "+STR$(b%!64)+" title "+FNs(b%+76))
  IF fetched%=0 THEN PROCfetchfile(t%,b%!28)
WHEN 3
  PROClog("DataLoad your_ref "+STR$~(b%!12)+" type &"+STR$~(b%!40)+" size "+STR$(b%!36)+" "+FNs(b%+44))
  IF shown%=0 THEN PROCdisplay(t%,obj%)
ENDCASE
ENDPROC

DEF PROCfetch(t%,x0%,y0%,x1%,y1%)
searched%=1
b%!0=76 : b%!12=0 : b%!16=&546D5 : b%!20=7 : b%!24=1 : b%!28=0 : b%!32=0
b%!36=x0% : b%!40=y0% : b%!44=x1% : b%!48=y1%
b%!52=32 OR (1<<16) OR (1<<18) : b%!56=q% : b%!60=LEN($q%)
SYS "Wimp_SendMessage",18,b%,t%
PROClog("sent FetchData my_ref "+STR$~(b%!8)+" box and ""Derwent""")
ENDPROC

DEF PROCfetchfile(t%,o%)
fetched%=1 : obj%=o%
b%!0=44 : b%!12=0 : b%!16=&546D7 : b%!20=7 : b%!24=1 : b%!28=o% : b%!32=&C85 : b%!36=&1E3 : b%!40=-1
SYS "Wimp_SendMessage",18,b%,t%
PROClog("sent FetchFile obj "+STR$(o%)+" my_ref "+STR$~(b%!8))
ENDPROC

DEF PROCdisplay(t%,o%)
shown%=1
b%!0=60 : b%!12=0 : b%!16=&546D8 : b%!20=7 : b%!24=1 : b%!28=o% : b%!32=0
SYS "Wimp_SendMessage",17,b%,t%
PROClog("sent DisplayInfo obj "+STR$(o%))
ENDPROC

DEF FNs(a%)
LOCAL s$
WHILE ?a%<>0 AND LEN(s$)<200 : s$+=CHR$(?a%) : a%+=1 : ENDWHILE
=s$

DEF PROClog(s$)
LOCAL f%
f%=OPENUP(log$) : PTR#f%=EXT#f% : BPUT#f%,s$ : CLOSE#f%
ENDPROC

DEF PROCerr
PROClog("error "+REPORT$+" at "+STR$(ERL))
ENDPROC
