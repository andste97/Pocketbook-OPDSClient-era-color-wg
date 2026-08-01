PS C:\Users\andre\IdeaProjects\Pocketbook-OPDSClient-era-color-wg> docker run --rm -v "${PWD}:/project" -w /project andi97/pocketbook-dev-docker:6.8-b300-r1 `  -lc 'export LD_LIBRARY_PATH="$SDK_BASE/usr/lib"; "$SDK_BASE/usr/bin/arm-obreey-linux-gnueabi-readelf" -V OPDSClient.app'

Version symbols section '.gnu.version' contains 150 entries:
 Addr: 0000000000011688  Offset: 0x001688  Link: 4 (.dynsym)
  000:   0 (*local*)       2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  004:   2 (GLIBC_2.4)     1 (*global*)      0 (*local*)       0 (*local*)    
  008:   0 (*local*)       0 (*local*)       0 (*local*)       2 (GLIBC_2.4)  
  00c:   1 (*global*)      0 (*local*)       2 (GLIBC_2.4)     0 (*local*)    
  010:   2 (GLIBC_2.4)     0 (*local*)       0 (*local*)       2 (GLIBC_2.4)  
  014:   0 (*local*)       0 (*local*)       2 (GLIBC_2.4)     2 (GLIBC_2.4)  
  018:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  01c:   2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)     3 (LIBXML2_2.4.30) 
  020:   1 (*global*)      2 (GLIBC_2.4)     0 (*local*)       0 (*local*)    
  024:   2 (GLIBC_2.4)     0 (*local*)       0 (*local*)       0 (*local*)    
  028:   1 (*global*)      0 (*local*)       0 (*local*)       3 (LIBXML2_2.4.30) 
  02c:   1 (*global*)      0 (*local*)       2 (GLIBC_2.4)     0 (*local*)    
  030:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  034:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)    
  038:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     2 (GLIBC_2.4)     2 (GLIBC_2.4)  
  03c:   0 (*local*)       2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)    
  040:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  044:   1 (*global*)      2 (GLIBC_2.4)     0 (*local*)       3 (LIBXML2_2.4.30) 
  048:   0 (*local*)       2 (GLIBC_2.4)     2 (GLIBC_2.4)     2 (GLIBC_2.4)  
  04c:   2 (GLIBC_2.4)     0 (*local*)       4 (GLIBC_2.4)     1 (*global*)   
  050:   0 (*local*)       3 (LIBXML2_2.4.30)    0 (*local*)       2 (GLIBC_2.4)  
  054:   0 (*local*)       2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)    
  058:   0 (*local*)       2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  05c:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     3 (LIBXML2_2.4.30)    5 (LIBXML2_2.6.0)
  060:   0 (*local*)       1 (*global*)      0 (*local*)       3 (LIBXML2_2.4.30) 
  064:   0 (*local*)       2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)  
  068:   2 (GLIBC_2.4)     0 (*local*)       2 (GLIBC_2.4)     3 (LIBXML2_2.4.30) 
  06c:   3 (LIBXML2_2.4.30)    0 (*local*)       2 (GLIBC_2.4)     2 (GLIBC_2.4)  
  070:   0 (*local*)       2 (GLIBC_2.4)     3 (LIBXML2_2.4.30)    0 (*local*)    
  074:   0 (*local*)       2 (GLIBC_2.4)     6 (GLIBC_2.4)     2 (GLIBC_2.4)  
  078:   3 (LIBXML2_2.4.30)    2 (GLIBC_2.4)     2 (GLIBC_2.4)     0 (*local*)    
  07c:   0 (*local*)       0 (*local*)       2 (GLIBC_2.4)     0 (*local*)    
  080:   2 (GLIBC_2.4)     2 (GLIBC_2.4)     1 (*global*)      2 (GLIBC_2.4)  
  084:   0 (*local*)       2 (GLIBC_2.4)     3 (LIBXML2_2.4.30)    2 (GLIBC_2.4)  
  088:   2 (GLIBC_2.4)     0 (*local*)       0 (*local*)       2 (GLIBC_2.4)  
  08c:   1 (*global*)      2 (GLIBC_2.4)     0 (*local*)       6 (GLIBC_2.4)  
  090:   0 (*local*)       0 (*local*)       0 (*local*)       0 (*local*)    
  094:   4 (GLIBC_2.4)     0 (*local*)    

Version needs section '.gnu.version_r' contains 4 entries:
 Addr: 0x00000000000117b4  Offset: 0x0017b4  Link: 5 (.dynstr)
  000000: Version: 1  File: libm.so.6  Cnt: 1
  0x0010:   Name: GLIBC_2.4  Flags: none  Version: 6
  0x0020: Version: 1  File: libdl.so.2  Cnt: 1
  0x0030:   Name: GLIBC_2.4  Flags: none  Version: 4
  0x0040: Version: 1  File: libxml2.so.2  Cnt: 2
  0x0050:   Name: LIBXML2_2.6.0  Flags: none  Version: 5
  0x0060:   Name: LIBXML2_2.4.30  Flags: none  Version: 3
  0x0070: Version: 1  File: libc.so.6  Cnt: 1
  0x0080:   Name: GLIBC_2.4  Flags: none  Version: 2