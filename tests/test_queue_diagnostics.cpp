#include "QueueDiagnostics.h"
#include <cassert>
#include <cstdio>
using D=motion::QueueDiagnostics;
int main() {
    D d;d.beginRun(1);
    d.submit(1,1,1,1,0xCD,0,true);d.response(1,0xCD,0xE2,10);
    assert(d.alert() && d.alert()->code==0xE2);
    d.response(1,0xCD,2,20);assert(d.at(0).confirmation==D::Confirmation::Rejected);
    for(unsigned i=0;i<70;++i)d.submit(1,2+i,2,2,0xCD,100+i,true);
    assert(d.count()==64 && d.dropped()==7 && d.alert()->line==1);
    d.response(2,0xCD,2,200);
    assert(d.at(63).confirmation==D::Confirmation::AssociationUncertain);
    d.beginRun(2);assert(!d.alert());
    d.submit(2,1,1,1,0xCD,300,true);
    assert(!d.alert());d.response(1,0xCD,2,301);
    assert(d.alert()->confirmation==D::Confirmation::AssociationUncertain);
    D fresh;fresh.beginRun(1);fresh.submit(1,1,1,3,0xCD,UINT32_MAX-100,true);
    fresh.poll(1900);assert(fresh.alert()->confirmation==D::Confirmation::Unconfirmed);
    fresh.response(3,0xCD,2,1901);
    assert(fresh.at(0).confirmation==D::Confirmation::Accepted);
    assert(fresh.alert()->confirmation==D::Confirmation::Unconfirmed); // historical alert remains
    D external;external.beginRun(1);external.submit(1,1,1,1,0xCD,1,true);
    external.invalidate(1);external.response(1,0xCD,2,2);
    assert(external.at(0).confirmation==D::Confirmation::AssociationUncertain);
    external.submit(1,1,2,0,0xFF,3,true,true);external.poll(3000);
    assert(external.at(1).confirmation==D::Confirmation::RawSubmitted);
    external.submit(1,1,3,1,0xCD,3001,true);external.poll(5001);
    assert(external.at(2).confirmation==D::Confirmation::Unconfirmed);
    D home;home.beginRun(1);home.submit(1,1,1,2,0x9A,0,true);home.response(2,0x9A,0x12,10);home.poll(3000);
    assert(home.at(0).confirmation==D::Confirmation::NoMotionReported && !home.alert());
    puts("PASS passive diagnostics: bounded retention, sticky rejection, late ACK ambiguity across runs, timeout wrap");
}
