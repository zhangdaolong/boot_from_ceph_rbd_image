#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdWire.h"
#include "CephRbdCrush.h"
#include "CephRbdCrushLb.h"

#define CRUSH_HASH_SEED  1315423911U
#define CRUSH_ITEM_NONE  ((INT32)0x80000000U)

STATIC
VOID
CephRbdCrushMix (
  IN OUT UINT32  *A,
  IN OUT UINT32  *B,
  IN OUT UINT32  *C
  )
{
  UINT32  AValue;
  UINT32  BValue;
  UINT32  CValue;

  AValue = *A;
  BValue = *B;
  CValue = *C;

  AValue = AValue - BValue;  AValue = AValue - CValue;  AValue ^= CValue >> 13;
  BValue = BValue - CValue;  BValue = BValue - AValue;  BValue ^= AValue << 8;
  CValue = CValue - AValue;  CValue = CValue - BValue;  CValue ^= BValue >> 13;
  AValue = AValue - BValue;  AValue = AValue - CValue;  AValue ^= CValue >> 12;
  BValue = BValue - CValue;  BValue = BValue - AValue;  BValue ^= AValue << 16;
  CValue = CValue - AValue;  CValue = CValue - BValue;  CValue ^= BValue >> 5;
  AValue = AValue - BValue;  AValue = AValue - CValue;  AValue ^= CValue >> 3;
  BValue = BValue - CValue;  BValue = BValue - AValue;  BValue ^= AValue << 10;
  CValue = CValue - AValue;  CValue = CValue - BValue;  CValue ^= BValue >> 15;

  *A = AValue;
  *B = BValue;
  *C = CValue;
}

UINT32
CephRbdCrushStrHash (
  IN CONST CHAR8  *Name,
  IN UINTN        NameLength
  )
{
  CONST UINT8  *Key;
  UINT32       A;
  UINT32       B;
  UINT32       C;
  UINT32       Length;
  UINT32       Word;

  if ((Name == NULL) || (NameLength > MAX_UINT32)) {
    return 0;
  }

  Key = (CONST UINT8 *)Name;
  Length = (UINT32)NameLength;
  A = 0x9e3779b9U;
  B = A;
  C = 0;

  while (Length >= 12) {
    CopyMem (&Word, Key, sizeof (Word));
    A += Word;
    CopyMem (&Word, Key + 4, sizeof (Word));
    B += Word;
    CopyMem (&Word, Key + 8, sizeof (Word));
    C += Word;
    CephRbdCrushMix (&A, &B, &C);
    Key += 12;
    Length -= 12;
  }

  C += (UINT32)NameLength;
  switch (Length) {
  case 11: C += (UINT32)Key[10] << 24;  // fall through
  case 10: C += (UINT32)Key[9] << 16;   // fall through
  case 9:  C += (UINT32)Key[8] << 8;    // fall through
  case 8:  B += (UINT32)Key[7] << 24;   // fall through
  case 7:  B += (UINT32)Key[6] << 16;   // fall through
  case 6:  B += (UINT32)Key[5] << 8;    // fall through
  case 5:  B += Key[4];                 // fall through
  case 4:  A += (UINT32)Key[3] << 24;   // fall through
  case 3:  A += (UINT32)Key[2] << 16;   // fall through
  case 2:  A += (UINT32)Key[1] << 8;    // fall through
  case 1:  A += Key[0];                 // fall through
  default: break;
  }

  CephRbdCrushMix (&A, &B, &C);
  return C;
}

UINT32
CephRbdCrushHash32_2 (
  IN UINT32  HashType,
  IN UINT32  A,
  IN UINT32  B
  )
{
  UINT32  Hash;
  UINT32  X;
  UINT32  Y;

  if (HashType != CEPH_RBD_CRUSH_HASH_RJENKINS1) {
    return 0;
  }

  Hash = CRUSH_HASH_SEED ^ A ^ B;
  X = 231232;
  Y = 1232;
  CephRbdCrushMix (&A, &B, &Hash);
  CephRbdCrushMix (&X, &A, &Hash);
  CephRbdCrushMix (&B, &Y, &Hash);
  return Hash;
}

STATIC
UINT32
CephRbdCrushHash32_3 (
  IN UINT32  HashType,
  IN UINT32  A,
  IN UINT32  B,
  IN UINT32  C
  )
{
  UINT32  Hash;
  UINT32  X;
  UINT32  Y;

  if (HashType != CEPH_RBD_CRUSH_HASH_RJENKINS1) {
    return 0;
  }

  Hash = CRUSH_HASH_SEED ^ A ^ B ^ C;
  X = 231232;
  Y = 1232;
  CephRbdCrushMix (&A, &B, &Hash);
  CephRbdCrushMix (&C, &X, &Hash);
  CephRbdCrushMix (&Y, &A, &Hash);
  CephRbdCrushMix (&B, &X, &Hash);
  CephRbdCrushMix (&Y, &C, &Hash);
  return Hash;
}

STATIC
UINTN
CephRbdCrushClz32 (
  IN UINT32  Value
  )
{
  UINTN  Count;

  if (Value == 0) {
    return 32;
  }

  Count = 0;
  while ((Value & 0x80000000U) == 0) {
    Value <<= 1;
    Count++;
  }

  return Count;
}

// 2^44 * log2(x + 1), using the exact Ceph tables.
STATIC
UINT64
CephRbdCrushLn (
  IN UINT32  Input
  )
{
  UINT32  X;
  UINT32  Exponent;
  UINT32  Bits;
  UINT32  Index1;
  UINT64  Result;
  UINT64  Lh;
  UINT64  Xl64;

  X = Input + 1;
  Exponent = 15;
  if ((X & 0x18000U) == 0) {
    Bits = (UINT32)(CephRbdCrushClz32 (X & 0x1ffffU) - 16);
    X <<= Bits;
    Exponent = 15 - Bits;
  }

  Index1 = (X >> 8) << 1;
  // Table indices are Index1-256 and Index1+1-256; the latter must stay
  // below 258 (the RH/LH table length).
  if ((Index1 < 256) || ((Index1 + 1) >= 514)) {
    return 0;
  }

  Result = (UINT64)Exponent << (12 + 32);
  Xl64 = ((UINT64)X * (UINT64)(UINT64)mCephRbdCrushRhLhTable[Index1 - 256]) >> 48;
  Lh = (UINT64)mCephRbdCrushRhLhTable[Index1 + 1 - 256] +
       (UINT64)mCephRbdCrushLlTable[Xl64 & 0xff];
  Result += Lh >> (48 - 12 - 32);
  return Result;
}

STATIC
INT64
CephRbdCrushStraw2Draw (
  IN UINT32  HashType,
  IN UINT32  X,
  IN INT32   Item,
  IN INT32   R,
  IN UINT32  Weight
  )
{
  UINT32  U;
  INT64   Ln;

  U = CephRbdCrushHash32_3 (HashType, X, (UINT32)Item, (UINT32)R) & 0xffff;
  Ln = (INT64)CephRbdCrushLn (U) - 0x1000000000000LL;
  return Ln / (INT64)Weight;
}

EFI_STATUS
CephRbdCrushParseMap (
  IN  CONST VOID          *Buffer,
  IN  UINTN               BufferLength,
  OUT CEPH_RBD_CRUSH_MAP   *Map
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            Value;
  UINT32            Alg;
  UINT32            BucketCount;
  UINT32            RuleCount;
  UINT32            I;
  UINT32            J;
  UINT32            Size;
  UINT32            Len;
  INT32             Signed;
  UINT16            U16;
  UINT8             U8;

  if ((Buffer == NULL) || (Map == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Map, sizeof (*Map));
  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (&Cursor, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Value != CEPH_RBD_CRUSH_MAGIC) {
    return EFI_COMPROMISED_DATA;
  }
  Status = CephWireReadLe32 (&Cursor, &Map->MaxBuckets);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Map->MaxRules);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Map->MaxDevices);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Map->MaxBuckets == 0) || (Map->MaxBuckets > CEPH_RBD_CRUSH_MAX_BUCKETS) ||
      (Map->MaxRules > CEPH_RBD_CRUSH_MAX_RULES))
  {
    return EFI_UNSUPPORTED;
  }

  BucketCount = 0;
  for (I = 0; I < Map->MaxBuckets; I++) {
    Status = CephWireReadLe32 (&Cursor, &Alg);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Alg == 0) {
      continue;
    }

    if (BucketCount >= CEPH_RBD_CRUSH_MAX_BUCKETS) {
      return EFI_BAD_BUFFER_SIZE;
    }

    ZeroMem (&Map->Buckets[BucketCount], sizeof (Map->Buckets[BucketCount]));
    Status = CephWireReadLe32 (&Cursor, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Map->Buckets[BucketCount].Id = (INT32)Value;
    Status = CephWireReadLe16 (&Cursor, &U16);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Map->Buckets[BucketCount].Type = U16;
    Status = CephWireReadU8 (&Cursor, &U8);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Map->Buckets[BucketCount].Alg = U8;
    Status = CephWireReadU8 (&Cursor, &U8);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Map->Buckets[BucketCount].Hash = U8;
    Status = CephWireReadLe32 (&Cursor, &Map->Buckets[BucketCount].Weight);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CephWireReadLe32 (&Cursor, &Size);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Size > CEPH_RBD_CRUSH_MAX_ITEMS) {
      return EFI_UNSUPPORTED;
    }
    Map->Buckets[BucketCount].Size = Size;

    for (J = 0; J < Size; J++) {
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Map->Buckets[BucketCount].Items[J] = (INT32)Value;
    }

    switch (Map->Buckets[BucketCount].Alg) {
    case CEPH_RBD_CRUSH_BUCKET_STRAW2:
      for (J = 0; J < Size; J++) {
        Status = CephWireReadLe32 (
                   &Cursor,
                   &Map->Buckets[BucketCount].ItemWeights[J]
                   );
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
      break;

    case CEPH_RBD_CRUSH_BUCKET_UNIFORM:
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      break;

    case CEPH_RBD_CRUSH_BUCKET_LIST:
      for (J = 0; J < Size; J++) {
        Status = CephWireReadLe32 (&Cursor, &Value);
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&Cursor, &Value);
        }
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
      break;

    case CEPH_RBD_CRUSH_BUCKET_TREE:
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      for (J = 0; J < Value; J++) {
        Status = CephWireReadLe32 (&Cursor, &Value);
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
      break;

    case CEPH_RBD_CRUSH_BUCKET_STRAW:
      for (J = 0; J < Size; J++) {
        Status = CephWireReadLe32 (&Cursor, &Value);
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&Cursor, &Value);
        }
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
      break;

    default:
      return EFI_UNSUPPORTED;
    }

    BucketCount++;
  }
  Map->BucketCount = BucketCount;

  RuleCount = 0;
  for (I = 0; I < Map->MaxRules; I++) {
    Status = CephWireReadLe32 (&Cursor, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Value == 0) {
      continue;
    }
    if (RuleCount >= CEPH_RBD_CRUSH_MAX_RULES) {
      return EFI_BAD_BUFFER_SIZE;
    }

    ZeroMem (&Map->Rules[RuleCount], sizeof (Map->Rules[RuleCount]));
    Status = CephWireReadLe32 (&Cursor, &Len);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Len > CEPH_RBD_CRUSH_MAX_STEPS) {
      return EFI_UNSUPPORTED;
    }
    Map->Rules[RuleCount].Len = Len;

    Status = CephWireReadU8 (&Cursor, &Map->Rules[RuleCount].Ruleset);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CephWireReadU8 (&Cursor, &Map->Rules[RuleCount].Type);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CephWireReadU8 (&Cursor, &U8);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CephWireReadU8 (&Cursor, &U8);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    for (J = 0; J < Len; J++) {
      Status = CephWireReadLe32 (&Cursor, &Map->Rules[RuleCount].Steps[J].Op);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Map->Rules[RuleCount].Steps[J].Arg1 = (INT32)Value;
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Map->Rules[RuleCount].Steps[J].Arg2 = (INT32)Value;
    }
    RuleCount++;
  }
  Map->RuleCount = RuleCount;

  // Skip the name maps. They are map<int,string> in all supported versions.
  for (I = 0; I < 3; I++) {
    Status = CephWireReadLe32 (&Cursor, &Len);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Len > 1024) {
      return EFI_BAD_BUFFER_SIZE;
    }
    for (J = 0; J < Len; J++) {
      Status = CephWireReadLe32 (&Cursor, &Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = CephWireReadLe32 (&Cursor, &Size);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = CephWireSkip (&Cursor, Size);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  Status = CephWireReadLe32 (&Cursor, &Value);   // choose_local_tries
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Value);   // choose_local_fallback_tries
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Map->ChooseTotalTries);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Value);   // chooseleaf_descend_once
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadU8 (&Cursor, &Map->ChooseleafVaryR);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadU8 (&Cursor, &Map->StrawCalcVersion);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadLe32 (&Cursor, &Map->AllowedBucketAlgs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephWireReadU8 (&Cursor, &Map->ChooseleafStable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Map->ChooseTotalTries == 0) {
    Map->ChooseTotalTries = 50;
  }

  // Touch Signed to make the intent of the signed reads explicit in this
  // decoder; all signed wire fields above are read as UINT32 and cast.
  Signed = 0;
  (VOID)Signed;
  return EFI_SUCCESS;
}

STATIC
CONST CEPH_RBD_CRUSH_BUCKET *
CephRbdCrushFindBucket (
  IN CONST CEPH_RBD_CRUSH_MAP  *Map,
  IN INT32                     Id
  )
{
  UINT32  Index;

  if (Id >= 0) {
    return NULL;
  }

  for (Index = 0; Index < Map->BucketCount; Index++) {
    if (Map->Buckets[Index].Id == Id) {
      return &Map->Buckets[Index];
    }
  }

  return NULL;
}

STATIC
INT32
CephRbdCrushChooseFromBucket (
  IN CONST CEPH_RBD_CRUSH_BUCKET  *Bucket,
  IN UINT32                       X,
  IN INT32                        R
  )
{
  UINT32  Index;
  UINT32  Best;
  INT64   Draw;
  INT64   BestDraw;

  if ((Bucket == NULL) || (Bucket->Size == 0)) {
    return CRUSH_ITEM_NONE;
  }

  if (Bucket->Alg != CEPH_RBD_CRUSH_BUCKET_STRAW2) {
    return CRUSH_ITEM_NONE;
  }

  Best = 0;
  BestDraw = 0;
  for (Index = 0; Index < Bucket->Size; Index++) {
    if (Bucket->ItemWeights[Index] == 0) {
      Draw = (INT64)0x8000000000000000ULL;
    } else {
      Draw = CephRbdCrushStraw2Draw (
               Bucket->Hash,
               X,
               Bucket->Items[Index],
               R,
               Bucket->ItemWeights[Index]
               );
    }
    if ((Index == 0) || (Draw > BestDraw)) {
      Best = Index;
      BestDraw = Draw;
    }
  }

  return Bucket->Items[Best];
}

STATIC
BOOLEAN
CephRbdCrushIsOut (
  IN CONST UINT32  *OsdWeights,
  IN UINT32        MaxDevices,
  IN INT32         Osd
  )
{
  if ((OsdWeights == NULL) || (Osd < 0) || ((UINT32)Osd >= MaxDevices)) {
    return FALSE;
  }

  return (OsdWeights[Osd] == 0);
}

EFI_STATUS
CephRbdCrushMapRule (
  IN  CONST CEPH_RBD_CRUSH_MAP  *Map,
  IN  UINT32                    RuleId,
  IN  UINT32                    Pps,
  IN  UINT32                    Size,
  IN  CONST UINT32              *OsdWeights  OPTIONAL,
  OUT INT32                     *OsdIds,
  IN  UINTN                     OsdIdsCapacity,
  OUT UINTN                     *OsdIdCount
  )
{
  CONST CEPH_RBD_CRUSH_RULE    *Rule;
  CONST CEPH_RBD_CRUSH_BUCKET  *In;
  UINT32                        StepIndex;
  UINT32                        Rep;
  UINT32                        Ftal;
  UINT32                        OutCount;
  INT32                         Item;
  INT32                         R;
  INT32                         TargetType;
  INT32                         NumRep;
  UINT32                        Index;
  BOOLEAN                       Collide;

  if ((Map == NULL) || (OsdIds == NULL) || (OsdIdCount == NULL) ||
      (Size == 0) || (Size > CEPH_RBD_CRUSH_MAX_RESULT) ||
      (OsdIdsCapacity < Size))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (RuleId >= Map->RuleCount) {
    return EFI_NOT_FOUND;
  }

  Rule = &Map->Rules[RuleId];
  OutCount = 0;
  In = NULL;

  for (StepIndex = 0; StepIndex < Rule->Len; StepIndex++) {
    switch (Rule->Steps[StepIndex].Op) {
    case CEPH_RBD_CRUSH_RULE_TAKE:
      In = CephRbdCrushFindBucket (Map, Rule->Steps[StepIndex].Arg1);
      if (In == NULL) {
        return EFI_NOT_FOUND;
      }
      break;

    case CEPH_RBD_CRUSH_RULE_CHOOSE_FIRSTN:
    case CEPH_RBD_CRUSH_RULE_CHOOSELEAF_FIRSTN:
      if (In == NULL) {
        return EFI_INVALID_PARAMETER;
      }

      NumRep = Rule->Steps[StepIndex].Arg1;
      if (NumRep <= 0) {
        NumRep = (INT32)Size;
      }
      if ((UINT32)NumRep > CEPH_RBD_CRUSH_MAX_RESULT) {
        return EFI_UNSUPPORTED;
      }
      TargetType = Rule->Steps[StepIndex].Arg2;

      for (Rep = 0; Rep < (UINT32)NumRep; Rep++) {
        Ftal = 0;
        while (TRUE) {
          CONST CEPH_RBD_CRUSH_BUCKET  *Current;
          CONST CEPH_RBD_CRUSH_BUCKET  *ChosenBucket;
          BOOLEAN                       Retry;
          INT32                         ItemType;

          Current = In;
          Retry = FALSE;
          R = (INT32)Rep + (INT32)Ftal;

          while (TRUE) {
            ChosenBucket = NULL;
            Item = CephRbdCrushChooseFromBucket (Current, Pps, R);
            if (Item == CRUSH_ITEM_NONE) {
              return EFI_UNSUPPORTED;
            }

            if (Item >= 0) {
              ItemType = 0;
            } else {
              ChosenBucket = CephRbdCrushFindBucket (Map, Item);
              if (ChosenBucket == NULL) {
                return EFI_NOT_FOUND;
              }
              ItemType = (INT32)ChosenBucket->Type;
            }

            if (ItemType != TargetType) {
              if (Item >= 0) {
                return EFI_UNSUPPORTED;
              }
              Current = ChosenBucket;
              continue;
            }

            // CHOOSELEAF descends one more level to a device.
            if ((Rule->Steps[StepIndex].Op ==
                 CEPH_RBD_CRUSH_RULE_CHOOSELEAF_FIRSTN) &&
                (Item < 0))
            {
              Item = CephRbdCrushChooseFromBucket (ChosenBucket, Pps, R);
              if ((Item == CRUSH_ITEM_NONE) ||
                  ((UINT32)Item >= Map->MaxDevices))
              {
                Retry = TRUE;
                break;
              }
            }

            Collide = FALSE;
            for (Index = 0; Index < OutCount; Index++) {
              if (OsdIds[Index] == Item) {
                Collide = TRUE;
                break;
              }
            }

            if ((!Collide) &&
                CephRbdCrushIsOut (OsdWeights, Map->MaxDevices, Item))
            {
              Collide = TRUE;
            }

            if (Collide) {
              Ftal++;
              if (Ftal >= Map->ChooseTotalTries) {
                return EFI_DEVICE_ERROR;
              }
              Retry = TRUE;
              break;
            }

            OsdIds[OutCount++] = Item;
            break;
          }

          if (!Retry) {
            break;
          }
        }
      }
      break;

    case CEPH_RBD_CRUSH_RULE_EMIT:
      *OsdIdCount = OutCount;
      return EFI_SUCCESS;

    default:
      return EFI_UNSUPPORTED;
    }
  }

  *OsdIdCount = OutCount;
  return EFI_SUCCESS;
}
